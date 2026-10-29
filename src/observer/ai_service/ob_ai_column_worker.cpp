/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SERVER

#include "observer/ai_service/ob_ai_column_worker.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "share/ai_service/ob_ai_column_meta.h"
#include "share/ai_service/ob_ai_column_sql_utils.h"
#include "share/ob_server_struct.h"
#include "share/ob_shared_timer.h"
#include "share/rc/ob_server_runtime.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/allocator/page_arena.h"
#include "lib/time/ob_time_utility.h"
#include <inttypes.h>

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

namespace
{
const char *AI_TASK_TABLE = "oceanbase.__all_ai_column_task";
const int64_t AI_WORKER_INTERVAL_US = 100 * 1000L;
const int64_t AI_RETRY_DELAY_US = 500 * 1000L;
const int64_t AI_TASK_BATCH_SIZE = 64;

int append_sql_escaped(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if ('\'' == c) {
      ret = sql.append("\\'");
    } else if ('\\' == c) {
      ret = sql.append("\\\\");
    } else {
      ret = sql.append(value.ptr() + i, 1);
    }
  }
  return ret;
}

} // namespace

ObAiColumnWorker &ObAiColumnWorker::get_instance()
{
  static ObAiColumnWorker instance;
  return instance;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (is_start_) {
  } else if (OB_ISNULL(share::server_service<share::ObISharedTimer>())) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(share::server_service<share::ObISharedTimer>()->schedule(
                       *this, AI_WORKER_INTERVAL_US, true /*repeat*/))) {
    LOG_WARN("failed to schedule ai column worker", K(ret));
  } else {
    disable_timeout_check();
    is_start_ = true;
    LOG_INFO("ai column worker started");
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  if (is_start_) {
    share::server_service<share::ObISharedTimer>()->cancel_task(*this);
    is_start_ = false;
  }
}

void ObAiColumnWorker::runTimerTask()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<AiTask, AI_TASK_BATCH_SIZE> tasks;
  if (OB_ISNULL(GCTX.sql_proxy_) || OB_ISNULL(GCTX.schema_service_)) {
    // server is not ready yet
  } else if (OB_FAIL(fetch_tasks(alloc, tasks))) {
    LOG_WARN("failed to fetch ai column tasks", K(ret));
  } else if (tasks.empty()) {
    // nothing to do
  } else {
    int64_t i = 0;
    while (i < tasks.count()) {
      ObSEArray<AiTask, 4> group;
      const uint64_t table_id = tasks.at(i).table_id_;
      const ObString rowkey = tasks.at(i).rowkey_;
      // fetch_tasks orders complete row groups contiguously.
      while (OB_SUCC(ret) && i < tasks.count()
             && tasks.at(i).table_id_ == table_id && tasks.at(i).rowkey_ == rowkey) {
        ret = group.push_back(tasks.at(i++));
      }
      if (OB_SUCC(ret) && !group.empty()) {
        int tmp_ret = process_task_group(table_id, rowkey, group);
        if (OB_SUCCESS != tmp_ret) {
          LOG_WARN("failed to process ai column task group", K(tmp_ret), K(table_id), K(rowkey));
        }
      }
      if (OB_FAIL(ret)) { break; }
    }
  }
}

int ObAiColumnWorker::fetch_tasks(const ObIAllocator &unused, ObIArray<AiTask> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObIAllocator &task_alloc = const_cast<ObIAllocator &>(unused);
  if (OB_FAIL(sql.append_fmt(
        // Limit rows rather than individual columns: never split an atomic
        // result group at the batch boundary, including delayed retries.
        "SELECT t.table_id, t.column_id, t.rowkey_predicate, t.request_id, t.retry_count, t.state "
        "FROM %s t JOIN (SELECT table_id, rowkey_predicate FROM %s WHERE state <> 1 "
        "GROUP BY table_id, rowkey_predicate HAVING MAX(eligible_ts) <= now(6) "
        "ORDER BY MIN(eligible_ts), table_id, rowkey_predicate LIMIT %" PRId64 ") batch "
        "ON t.table_id = batch.table_id AND t.rowkey_predicate = batch.rowkey_predicate "
        "WHERE t.state <> 1 ORDER BY t.table_id, t.rowkey_predicate, t.column_id",
        AI_TASK_TABLE, AI_TASK_TABLE, AI_TASK_BATCH_SIZE))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          AiTask task;
          int64_t table_id = OB_INVALID_ID;
          int64_t column_id = OB_INVALID_ID;
          int64_t retry_count = 0;
          int64_t state = 0;
          ObString rowkey;
          ObString request_id;
          EXTRACT_INT_FIELD_MYSQL((*result), "table_id", table_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "column_id", column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "retry_count", retry_count, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "state", state, int64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL((*result), "rowkey_predicate", rowkey);
          EXTRACT_VARCHAR_FIELD_MYSQL((*result), "request_id", request_id);
          if (OB_FAIL(ret)) {
          } else {
            task.table_id_ = static_cast<uint64_t>(table_id);
            task.column_id_ = static_cast<uint64_t>(column_id);
            task.retry_count_ = retry_count;
            task.state_ = state;
            // the result object is released at the end of this scope, copy the strings
            if (OB_FAIL(ob_write_string(task_alloc, rowkey, task.rowkey_))
                || OB_FAIL(ob_write_string(task_alloc, request_id, task.request_id_))) {
            } else if (OB_FAIL(tasks.push_back(task))) {
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::delete_tasks(const uint64_t table_id,
                                   const ObString &rowkey,
                                   const ObIArray<AiTask> &group)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id = %" PRIu64 " AND rowkey_predicate = '",
                             AI_TASK_TABLE, table_id))) {
  } else if (OB_FAIL(append_sql_escaped(sql, rowkey))) {
  } else if (OB_FAIL(sql.append("' AND ("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    if (OB_FAIL(sql.append_fmt("%scolumn_id = %" PRIu64 " AND request_id = '",
                               0 == i ? "" : " OR ", group.at(i).column_id_))) {
    } else if (OB_FAIL(append_sql_escaped(sql, group.at(i).request_id_))) {
    } else if (OB_FAIL(sql.append("'"))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append(")"))) {
  } else if (OB_SUCC(ret) && OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
  }
  return ret;
}

int ObAiColumnWorker::handle_task_failure(const uint64_t table_id,
                                          const ObString &rowkey,
                                          const ObIArray<AiTask> &group,
                                          const ObIArray<ObAiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const AiTask &task = group.at(i);
    const ObAiColumnMeta *meta = NULL;
    for (int64_t j = 0; j < metas.count() && NULL == meta; ++j) {
      if (metas.at(j).column_id_ == task.column_id_) {
        meta = &metas.at(j);
      }
    }
    const int64_t max_retries = NULL != meta ? meta->max_retries_ : 0;
    if (task.retry_count_ >= max_retries) {
      // terminal failure: drop the task and mark the column as FAILED
      int64_t affected_rows = 0;
      ObSchemaGetterGuard guard;
      const ObTableSchema *table_schema = NULL;
      const ObDatabaseSchema *db_schema = NULL;
      if (NULL != meta) {
        const ObColumnSchemaV2 *state_col = NULL;
        const ObColumnSchemaV2 *request_col = NULL;
        if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
        } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema)) || OB_ISNULL(table_schema)) {
          if (OB_SUCC(ret)) { ret = OB_ERR_UNEXPECTED; }
        } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), db_schema))
                   || OB_ISNULL(db_schema)
                   || OB_ISNULL(state_col = table_schema->get_column_schema(meta->state_column_id_))
                   || OB_ISNULL(request_col = table_schema->get_column_schema(meta->request_column_id_))) {
          if (OB_SUCC(ret)) { ret = OB_ERR_UNEXPECTED; }
        } else if (OB_FAIL(sql.append("UPDATE "))
                   || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_qualified_identifier(sql,
                                db_schema->get_database_name_str(), table_schema->get_table_name_str()))
                   || OB_FAIL(sql.append(" SET "))
                   || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_escaped_identifier(sql, state_col->get_column_name_str()))
                   || OB_FAIL(sql.append(" = 3 WHERE "))) {
        } else if (OB_FAIL(sql.append(rowkey))) {
        } else if (OB_FAIL(sql.append(" AND "))
                   || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_escaped_identifier(sql, request_col->get_column_name_str()))
                   || OB_FAIL(sql.append(" = '"))) {
        } else if (OB_FAIL(append_sql_escaped(sql, task.request_id_))) {
        } else if (OB_FAIL(sql.append("'"))) {
        } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
          LOG_WARN("failed to mark ai column failure", K(ret), K(sql));
        }
      }
      sql.reuse();
      if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(
            "DELETE FROM %s WHERE table_id = %" PRIu64 " AND column_id = %" PRIu64 " AND rowkey_predicate = '",
            AI_TASK_TABLE, table_id, task.column_id_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(append_sql_escaped(sql, rowkey))) {
      } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("' AND request_id = '%.*s'", task.request_id_.length(), task.request_id_.ptr()))) {
      } else if (OB_SUCC(ret) && OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
      }
    } else {
      int64_t affected_rows = 0;
      sql.reuse();
      if (OB_FAIL(sql.append_fmt(
            "UPDATE %s SET state = 3, retry_count = retry_count + 1, "
            "eligible_ts = now(6) + interval %" PRId64 " microsecond "
            "WHERE table_id = %" PRIu64 " AND column_id = %" PRIu64 " AND rowkey_predicate = '",
            AI_TASK_TABLE, AI_RETRY_DELAY_US, table_id, task.column_id_))) {
      } else if (OB_FAIL(append_sql_escaped(sql, rowkey))) {
      } else if (OB_FAIL(sql.append_fmt("' AND request_id = '%.*s'", task.request_id_.length(), task.request_id_.ptr()))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::process_task_group(const uint64_t table_id,
                                         const ObString &rowkey,
                                         const ObIArray<AiTask> &group)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *db_schema = NULL;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<ObAiColumnMeta, 4> all_metas;
  ObSEArray<ObAiColumnMeta, 4> metas;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema)) || OB_ISNULL(table_schema)) {
    LOG_WARN("ai worker: table not found, drop tasks", K(ret), K(table_id));
    ret = OB_SUCCESS;
    int tmp_ret = delete_tasks(table_id, rowkey, group);
    LOG_WARN("ai worker: delete orphan tasks", K(tmp_ret));
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), db_schema))
             || OB_ISNULL(db_schema)) {
  } else if (OB_FAIL(ObAiColumnMetaCache::get_instance().ensure_table_metas(
                       table_id, 1000000, alloc, all_metas))) {
  } else {
    ObSqlString select_sql;
    ObSqlString update_sql;
    if (OB_FAIL(select_sql.append_fmt("SELECT "))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      const AiTask &task = group.at(i);
      ObAiColumnMeta meta;
      for (int64_t j = 0; j < all_metas.count(); ++j) {
        if (all_metas.at(j).column_id_ == task.column_id_) {
          meta = all_metas.at(j);
        }
      }
      if (OB_INVALID_ID == meta.column_id_
          || OB_ISNULL(table_schema->get_column_schema(meta.column_id_))) {
        // metadata gone (column dropped): drop the task later
        LOG_INFO("ai worker: stale task without metadata", K(table_id), K(task.column_id_));
      } else {
        if (OB_FAIL(select_sql.append_fmt("%s(%.*s)", 0 == metas.count() ? "" : ", ",
                                          meta.expr_.length(), meta.expr_.ptr()))) {
        } else if (OB_FAIL(metas.push_back(meta))) {
        }
      }
    }
    if (OB_SUCC(ret) && metas.empty()) {
      int tmp_ret = delete_tasks(table_id, rowkey, group);
      LOG_WARN("ai worker: no living ai column for tasks", K(tmp_ret));
    } else if (OB_SUCC(ret) && (OB_FAIL(select_sql.append(" FROM "))
               || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_qualified_identifier(select_sql,
                            db_schema->get_database_name_str(), table_schema->get_table_name_str()))
               || OB_FAIL(select_sql.append(" WHERE ")))) {
    } else if (OB_SUCC(ret) && OB_FAIL(select_sql.append(rowkey))) {
    } else if (OB_SUCC(ret)) {
      ObAIFuncClient::SingleAttemptGuard single_attempt;
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        ObMySQLResult *result = NULL;
        bool row_exists = true;
        bool value_ok = true;
        if (OB_FAIL(GCTX.sql_proxy_->read(res, select_sql.ptr()))) {
          LOG_WARN("ai worker: evaluate ai expression failed", K(ret), K(select_sql));
          value_ok = false;
          if (OB_FAIL(handle_task_failure(table_id, rowkey, group, metas))) {
          }
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ITER_END == (ret = result->next())) {
          ret = OB_SUCCESS;
          row_exists = false;
          int tmp_ret = delete_tasks(table_id, rowkey, group);
          LOG_WARN("ai worker: row gone, drop tasks", K(tmp_ret), K(table_id), K(rowkey));
        } else if (OB_FAIL(ret)) {
        } else if (OB_FAIL(update_sql.append("UPDATE "))
                   || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_qualified_identifier(update_sql,
                                db_schema->get_database_name_str(), table_schema->get_table_name_str()))
                   || OB_FAIL(update_sql.append(" SET "))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
            ObString value;
            if (OB_FAIL(result->get_varchar(i, value))) {
              value_ok = false;
              break;
            } else if ((i > 0 && OB_FAIL(update_sql.append(", ")))
                       || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_escaped_identifier(update_sql,
                                    table_schema->get_column_schema(metas.at(i).column_id_)->get_column_name_str()))
                       || OB_FAIL(update_sql.append(" = CONVERT("))) {
            } else if (OB_FAIL(share::append_ai_hex_literal(update_sql, value))) {
            } else if (OB_FAIL(update_sql.append(" USING utf8mb4)"))) {
            } else {
              const ObColumnSchemaV2 *state_column =
                  table_schema->get_column_schema(metas.at(i).state_column_id_);
              if (OB_ISNULL(state_column)) {
                ret = OB_ERR_UNEXPECTED;
              } else if (OB_FAIL(update_sql.append(", "))
                         || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_escaped_identifier(update_sql,
                                      state_column->get_column_name_str()))
                         || OB_FAIL(update_sql.append(" = 2"))) {
              }
            }
          }
          if (OB_SUCC(ret) && !value_ok) {
            ret = OB_ERR_UNEXPECTED;
          }
        }
        if (OB_SUCC(ret) && row_exists && value_ok) {
          int64_t affected_rows = 0;
          if (OB_FAIL(update_sql.append(" WHERE "))) {
          } else if (OB_FAIL(update_sql.append(rowkey))) {
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
            const AiTask *task = NULL;
            for (int64_t j = 0; j < group.count(); ++j) {
              if (group.at(j).column_id_ == metas.at(i).column_id_) {
                task = &group.at(j);
              }
            }
            if (OB_ISNULL(task)) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_ISNULL(table_schema->get_column_schema(metas.at(i).request_column_id_))) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_FAIL(update_sql.append(" AND "))
                       || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_escaped_identifier(update_sql,
                                    table_schema->get_column_schema(metas.at(i).request_column_id_)->get_column_name_str()))
                       || OB_FAIL(update_sql.append(" = '"))
                       || OB_FAIL(append_sql_escaped(update_sql, task->request_id_))
                       || OB_FAIL(update_sql.append("'"))) {
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(GCTX.sql_proxy_->write(update_sql.ptr(), affected_rows))) {
            LOG_WARN("ai worker: write result failed", K(ret), K(update_sql));
            int tmp_ret = handle_task_failure(table_id, rowkey, group, metas);
            LOG_WARN("ai worker: handle failure", K(tmp_ret));
          } else if (OB_SUCC(ret) && affected_rows > 0) {
            int tmp_ret = delete_tasks(table_id, rowkey, group);
            LOG_WARN("ai worker: delete finished tasks", K(tmp_ret), K(affected_rows));
          } else if (OB_SUCC(ret)) {
            // One request may have changed while another column in the same
            // group is still pending. Keep the group for a fresh snapshot;
            // deleting it here would lose the unchanged column's task. A
            // previous execution may also have committed the results and then
            // failed before deleting tasks, so clean that case up explicitly.
            ObSqlString ready_sql;
            if (OB_FAIL(ready_sql.append("SELECT 1 FROM "))
                || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_qualified_identifier(ready_sql,
                             db_schema->get_database_name_str(), table_schema->get_table_name_str()))
                || OB_FAIL(ready_sql.append(" WHERE "))
                || OB_FAIL(ready_sql.append(rowkey))) {
            }
            for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
              const AiTask *task = NULL;
              const ObColumnSchemaV2 *state = table_schema->get_column_schema(metas.at(i).state_column_id_);
              const ObColumnSchemaV2 *request = table_schema->get_column_schema(metas.at(i).request_column_id_);
              for (int64_t j = 0; j < group.count() && NULL == task; ++j) {
                if (group.at(j).column_id_ == metas.at(i).column_id_) { task = &group.at(j); }
              }
              if (OB_ISNULL(task) || OB_ISNULL(state) || OB_ISNULL(request)) {
                ret = OB_ERR_UNEXPECTED;
              } else if (OB_FAIL(ready_sql.append(" AND "))
                         || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_escaped_identifier(ready_sql, state->get_column_name_str()))
                         || OB_FAIL(ready_sql.append(" = 2 AND "))
                         || OB_FAIL(::oceanbase::sql::ObResolverUtils::append_escaped_identifier(ready_sql, request->get_column_name_str()))
                         || OB_FAIL(ready_sql.append(" = '"))
                         || OB_FAIL(append_sql_escaped(ready_sql, task->request_id_))
                         || OB_FAIL(ready_sql.append("'"))) {
              }
            }
            if (OB_SUCC(ret)) {
              ObMySQLProxy::MySQLResult ready_result;
              if (OB_FAIL(GCTX.sql_proxy_->read(ready_result, ready_sql.ptr()))) {
              } else if (OB_ISNULL(ready_result.get_result())) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                const int next_ret = ready_result.get_result()->next();
                if (OB_SUCCESS == next_ret) {
                  ret = delete_tasks(table_id, rowkey, group);
                } else if (OB_ITER_END != next_ret) {
                  ret = next_ret;
                }
              }
            }
          }
        } else if (OB_FAIL(ret) && row_exists && !value_ok) {
          LOG_WARN("ai worker: ai result is null or missing", K(ret), K(table_id), K(rowkey));
          ret = OB_SUCCESS;
          int tmp_ret = handle_task_failure(table_id, rowkey, group, metas);
          LOG_WARN("ai worker: handle failure", K(tmp_ret));
        }
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
