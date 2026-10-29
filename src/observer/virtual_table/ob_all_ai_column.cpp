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
#include "observer/virtual_table/ob_all_ai_column.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_table_schema.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

ObAllAIColumn::ObAllAIColumn()
    : databases_(),
      tables_(),
      db_idx_(0),
      table_idx_(0),
      column_idx_(0),
      is_inited_(false)
{
}

ObAllAIColumn::~ObAllAIColumn()
{
  reset();
}

void ObAllAIColumn::reset()
{
  databases_.reset();
  tables_.reset();
  db_idx_ = 0;
  table_idx_ = 0;
  column_idx_ = 0;
  is_inited_ = false;
  ObVirtualTableIterator::reset();
}

int ObAllAIColumn::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(allocator_) || OB_ISNULL(schema_guard_)) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "data member not init", K(ret), KP(allocator_), KP(schema_guard_));
  } else if (OB_FAIL(schema_guard_->get_database_schemas_in_runtime(databases_))) {
    SERVER_LOG(WARN, "failed to get databases", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAllAIColumn::load_next_table()
{
  int ret = OB_SUCCESS;
  tables_.reuse();
  table_idx_ = 0;
  column_idx_ = 0;
  while (OB_SUCC(ret) && db_idx_ < databases_.count()) {
    const ObSimpleDatabaseSchema *db_schema = databases_.at(db_idx_);
    ++db_idx_;
    if (OB_ISNULL(db_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(schema_guard_->get_table_schemas_in_database(
                 db_schema->get_database_id(), tables_))) {
      SERVER_LOG(WARN, "failed to get table schemas", K(ret), K(db_schema->get_database_id()));
    } else if (!tables_.empty()) {
      break;
    }
  }
  if (OB_SUCC(ret) && db_idx_ >= databases_.count()) {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObAllAIColumn::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    bool found = false;
    while (OB_SUCC(ret) && !found) {
      if (db_idx_ >= databases_.count()) {
        ret = OB_ITER_END;
      } else if (table_idx_ >= tables_.count()) {
        if (OB_FAIL(load_next_table())) {
          if (OB_ITER_END != ret) {
            SERVER_LOG(WARN, "failed to load next table", K(ret));
          }
        }
      } else {
        const ObTableSchema *table_schema = tables_.at(table_idx_);
        if (OB_ISNULL(table_schema)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (!table_schema->is_user_table()) {
          ++table_idx_;
          column_idx_ = 0;
        } else if (column_idx_ >= table_schema->get_column_count()) {
          ++table_idx_;
          column_idx_ = 0;
        } else {
          const ObColumnSchemaV2 *column = table_schema->get_column_schema_by_idx(column_idx_++);
          if (OB_NOT_NULL(column) && column->is_ai_column()) {
            if (OB_FAIL(fill_cells(*table_schema, *column))) {
            } else {
              row = &cur_row_;
              found = true;
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObAllAIColumn::fill_cells(const ObTableSchema &table_schema,
                              const ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  const int64_t col_count = output_column_ids_.count();
  for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
    uint64_t col_id = output_column_ids_.at(i);
    switch (col_id) {
      case TABLE_ID:
        cur_row_.cells_[i].set_int(static_cast<int64_t>(table_schema.get_table_id()));
        break;
      case COLUMN_ID:
        cur_row_.cells_[i].set_int(static_cast<int64_t>(column.get_column_id()));
        break;
      case COLUMN_NAME:
        cur_row_.cells_[i].set_varchar(column.get_column_name_str());
        break;
      case MAX_RETRIES:
        cur_row_.cells_[i].set_int(column.get_ai_max_retries());
        break;
      case SYNC_MODE:
        cur_row_.cells_[i].set_int(column.get_ai_sync_mode());
        break;
      case STATE_COLUMN_ID:
        cur_row_.cells_[i].set_int(static_cast<int64_t>(column.get_ai_state_column_id()));
        break;
      case REQUEST_COLUMN_ID:
        cur_row_.cells_[i].set_int(static_cast<int64_t>(column.get_ai_request_column_id()));
        break;
      case RESULT_SPEC_COLUMN_ID:
        cur_row_.cells_[i].set_int(static_cast<int64_t>(column.get_ai_spec_column_id()));
        break;
      case VERSION_COLUMN_ID:
        cur_row_.cells_[i].set_int(static_cast<int64_t>(column.get_ai_version_column_id()));
        break;
      case NORMALIZED_EXPR:
        cur_row_.cells_[i].set_varchar(column.get_ai_column_expr());
        break;
      case PROMPT_HINT:
        cur_row_.cells_[i].set_varchar(column.get_ai_prompt_hint());
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
        SERVER_LOG(WARN, "unexpected column id", K(ret), K(col_id));
        break;
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
