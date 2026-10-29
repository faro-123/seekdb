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

#define USING_LOG_PREFIX SHARE

#include "share/ai_service/ob_ai_column_meta.h"
#include "share/ob_server_struct.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_print_utils.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace share
{

ObAiColumnMetaCache &ObAiColumnMetaCache::get_instance()
{
  static ObAiColumnMetaCache instance;
  return instance;
}

ObAiColumnMetaCache::ObAiColumnMetaCache()
  : lock_(),
    arena_(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE),
    metas_(),
    last_reload_ts_(0)
{
}

void ObAiColumnMetaCache::reset()
{
  common::SpinWLockGuard guard(lock_);
  metas_.reset();
  arena_.reset();
  ATOMIC_STORE(&last_reload_ts_, 0);
}

int ObAiColumnMetaCache::reload()
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  if (OB_ISNULL(sql_proxy)) {
    ret = OB_NOT_INIT;
    LOG_WARN("sql proxy is null", K(ret));
  } else if (OB_FAIL(sql.assign_fmt(
      "SELECT table_id, column_id, column_name, max_retries, sync_mode, "
      "state_column_id, request_column_id, result_spec_column_id, version_column_id, "
      "IFNULL(normalized_expr, '') AS normalized_expr, IFNULL(prompt_hint, '') AS prompt_hint "
      "FROM %s", OB_ALL_AI_COLUMN_TNAME))) {
  } else {
    common::SpinWLockGuard guard(lock_);
    metas_.reset();
    arena_.reset();
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_proxy->read(res, sql.ptr()))) {
        LOG_WARN("failed to read ai column metadata", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          int64_t table_id = OB_INVALID_ID;
          int64_t column_id = OB_INVALID_ID;
          int64_t max_retries = 0;
          int64_t sync_mode = 0;
          int64_t state_column_id = OB_INVALID_ID;
          int64_t request_column_id = OB_INVALID_ID;
          int64_t spec_column_id = OB_INVALID_ID;
          int64_t version_column_id = OB_INVALID_ID;
          ObString column_name;
          ObString expr;
          ObString hint;
          void *buf = NULL;
          ObAiColumnMeta *meta = NULL;
          EXTRACT_INT_FIELD_MYSQL((*result), "table_id", table_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "column_id", column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "max_retries", max_retries, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "sync_mode", sync_mode, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "state_column_id", state_column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "request_column_id", request_column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "result_spec_column_id", spec_column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL((*result), "version_column_id", version_column_id, int64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL((*result), "column_name", column_name);
          EXTRACT_VARCHAR_FIELD_MYSQL((*result), "normalized_expr", expr);
          EXTRACT_VARCHAR_FIELD_MYSQL((*result), "prompt_hint", hint);
          if (OB_FAIL(ret)) {
          } else if (OB_ISNULL(buf = arena_.alloc(sizeof(ObAiColumnMeta)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            meta = new (buf) ObAiColumnMeta();
            meta->table_id_ = static_cast<uint64_t>(table_id);
            meta->column_id_ = static_cast<uint64_t>(column_id);
            meta->max_retries_ = max_retries;
            meta->sync_mode_ = sync_mode;
            meta->state_column_id_ = static_cast<uint64_t>(state_column_id);
            meta->request_column_id_ = static_cast<uint64_t>(request_column_id);
            meta->spec_column_id_ = static_cast<uint64_t>(spec_column_id);
            meta->version_column_id_ = static_cast<uint64_t>(version_column_id);
            if (OB_FAIL(ob_write_string(arena_, column_name, meta->column_name_))
                || OB_FAIL(ob_write_string(arena_, expr, meta->expr_))
                || OB_FAIL(ob_write_string(arena_, hint, meta->prompt_hint_))) {
            } else if (OB_FAIL(metas_.push_back(meta))) {
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
    if (OB_SUCC(ret)) {
      ATOMIC_STORE(&last_reload_ts_, ObTimeUtility::current_time());
    }
  }
  return ret;
}

int ObAiColumnMetaCache::get_table_metas(const uint64_t table_id,
                                         ObIAllocator &alloc,
                                         ObIArray<ObAiColumnMeta> &metas) const
{
  int ret = OB_SUCCESS;
  common::SpinRLockGuard guard(lock_);
  metas.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < metas_.count(); ++i) {
    const ObAiColumnMeta *meta = metas_.at(i);
    ObAiColumnMeta copy;
    if (OB_ISNULL(meta)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (meta->table_id_ != table_id) {
      // do nothing
    } else if (FALSE_IT(copy = *meta)) {
    } else if (OB_FAIL(ob_write_string(alloc, meta->column_name_, copy.column_name_))
               || OB_FAIL(ob_write_string(alloc, meta->expr_, copy.expr_))
               || OB_FAIL(ob_write_string(alloc, meta->prompt_hint_, copy.prompt_hint_))) {
    } else if (OB_FAIL(metas.push_back(copy))) {
    }
  }
  return ret;
}

int ObAiColumnMetaCache::ensure_table_metas(const uint64_t table_id,
                                            const int64_t min_interval_us,
                                            ObIAllocator &alloc,
                                            ObIArray<ObAiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  bool need_reload = false;
  {
    common::SpinRLockGuard guard(lock_);
    need_reload = metas_.empty()
        || ObTimeUtility::current_time() - last_reload_ts_ >= min_interval_us;
    if (!need_reload) {
      bool found = false;
      for (int64_t i = 0; !found && i < metas_.count(); ++i) {
        found = OB_NOT_NULL(metas_.at(i)) && metas_.at(i)->table_id_ == table_id;
      }
      need_reload = !found;
    }
  }
  if (need_reload && OB_FAIL(reload())) {
  } else if (OB_FAIL(get_table_metas(table_id, alloc, metas))) {
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
