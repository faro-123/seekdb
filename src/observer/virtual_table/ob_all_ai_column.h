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

#ifndef OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_H_
#define OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_H_

#include "observer/virtual_table/ob_virtual_table_iterator.h"
#include "lib/container/ob_se_array.h"
#include "share/ob_define.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObSimpleDatabaseSchema;
class ObTableSchema;
class ObColumnSchemaV2;
}
}
namespace observer
{

class ObAllAIColumn : public common::ObVirtualTableIterator
{
public:
  enum AIColumnColumn
  {
    TABLE_ID = common::OB_APP_MIN_COLUMN_ID,
    COLUMN_ID = common::OB_APP_MIN_COLUMN_ID + 1,
    COLUMN_NAME = common::OB_APP_MIN_COLUMN_ID + 2,
    MAX_RETRIES = common::OB_APP_MIN_COLUMN_ID + 3,
    SYNC_MODE = common::OB_APP_MIN_COLUMN_ID + 4,
    STATE_COLUMN_ID = common::OB_APP_MIN_COLUMN_ID + 5,
    REQUEST_COLUMN_ID = common::OB_APP_MIN_COLUMN_ID + 6,
    RESULT_SPEC_COLUMN_ID = common::OB_APP_MIN_COLUMN_ID + 7,
    VERSION_COLUMN_ID = common::OB_APP_MIN_COLUMN_ID + 8,
    NORMALIZED_EXPR = common::OB_APP_MIN_COLUMN_ID + 9,
    PROMPT_HINT = common::OB_APP_MIN_COLUMN_ID + 10
  };

  ObAllAIColumn();
  virtual ~ObAllAIColumn();
  virtual int inner_open() override;
  virtual int inner_get_next_row(common::ObNewRow *&row) override;
  virtual void reset() override;

private:
  int load_databases();
  int load_next_table();
  int fill_cells(const share::schema::ObTableSchema &table_schema,
                 const share::schema::ObColumnSchemaV2 &column);

private:
  common::ObSEArray<const share::schema::ObSimpleDatabaseSchema *, 16> databases_;
  common::ObSEArray<const share::schema::ObTableSchema *, 64> tables_;
  int64_t db_idx_;
  int64_t table_idx_;
  int64_t column_idx_;
  bool is_inited_;

  DISALLOW_COPY_AND_ASSIGN(ObAllAIColumn);
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_AI_COLUMN_H_
