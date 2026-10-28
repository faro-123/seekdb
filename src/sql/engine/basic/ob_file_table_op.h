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

#ifndef OCEANBASE_BASIC_OB_FILE_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FILE_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

class ObFileTableSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      path_(),
      format_(0),
      has_header_(true),
      column_names_(alloc),
      column_types_(alloc),
      column_nullables_(alloc),
      column_exprs_(alloc),
      column_idxs_(alloc),
      needed_cols_(alloc)
  {}
  // the resolved file path
  common::ObString path_;
  // FileScanFormat
  int32_t format_;
  bool has_header_;
  // the schema inferred by the resolver
  common::ObFixedArray<common::ObString, common::ObIAllocator> column_names_;
  common::ObFixedArray<int32_t, common::ObIAllocator> column_types_;
  common::ObFixedArray<int32_t, common::ObIAllocator> column_nullables_;
  // the output exprs of this table and their file column indexes
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  common::ObFixedArray<int32_t, common::ObIAllocator> column_idxs_;
  // the file columns which are actually referenced by the query
  common::ObFixedArray<int64_t, common::ObIAllocator> needed_cols_;
};

class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      scanner_(NULL),
      cells_(),
      is_inited_(false)
  {}
  virtual ~ObFileTableOp() {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  int init_scanner();

private:
  ObFileScanner *scanner_;
  common::ObArray<FileCell> cells_;
  bool is_inited_;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FILE_TABLE_OP_H_ */
