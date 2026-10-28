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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_table_op.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFileTableSpec, ObOpSpec), path_, format_, has_header_,
                    column_names_, column_types_, column_nullables_, column_exprs_,
                    column_idxs_, needed_cols_);

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_open())) {
  } else if (OB_FAIL(init_scanner())) {
  }
  return ret;
}

int ObFileTableOp::init_scanner()
{
  int ret = OB_SUCCESS;
  ObIAllocator &alloc = ctx_.get_allocator();
  ObSEArray<FileColumnMeta, 8> schema;
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_names_.count(); ++i) {
    FileColumnMeta meta;
    meta.name_ = MY_SPEC.column_names_.at(i);
    meta.type_ = static_cast<FileColumnType>(MY_SPEC.column_types_.at(i));
    meta.nullable_ = (0 != MY_SPEC.column_nullables_.at(i));
    if (OB_FAIL(schema.push_back(meta))) {
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(scanner_ = new (std::nothrow) ObFileScanner())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(scanner_->init(alloc,
                                      MY_SPEC.path_,
                                      static_cast<FileScanFormat>(MY_SPEC.format_),
                                      MY_SPEC.has_header_,
                                      schema,
                                      &MY_SPEC.needed_cols_))) {
    }
  }
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    if (OB_NOT_NULL(scanner_)) {
      delete scanner_;
      scanner_ = NULL;
    }
    cells_.reuse();
    if (OB_FAIL(init_scanner())) {
    }
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(scanner_)) {
    delete scanner_;
    scanner_ = NULL;
  }
  cells_.reuse();
  return ret;
}

void ObFileTableOp::destroy()
{
  if (OB_NOT_NULL(scanner_)) {
    delete scanner_;
    scanner_ = NULL;
  }
  cells_.reuse();
  ObOperator::destroy();
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(scanner_->next_row(cells_))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("failed to read the next file row", K(ret));
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      const int32_t col_idx = MY_SPEC.column_idxs_.at(i);
      if (OB_ISNULL(expr) || OB_UNLIKELY(col_idx < 0 || col_idx >= cells_.count())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid file table column", K(i), K(col_idx), K(cells_.count()));
      } else {
        const FileCell &cell = cells_.at(col_idx);
        ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
        if (cell.is_null_) {
          datum.set_null();
        } else {
          switch (expr->datum_meta_.type_) {
          case ObTinyIntType:
            datum.set_int(cell.bool_val_ ? 1 : 0);
            break;
          case ObIntType:
            datum.set_int(cell.int_val_);
            break;
          case ObDoubleType:
            datum.set_double(cell.dbl_val_);
            break;
          case ObDateType:
            datum.set_date(static_cast<int32_t>(cell.int_val_));
            break;
          case ObDateTimeType:
            datum.set_datetime(cell.int_val_);
            break;
          case ObVarcharType:
          case ObCharType:
          case ObLongTextType:
            datum.set_string(cell.str_);
            break;
          case ObNullType:
            datum.set_null();
            break;
          default: {
            ret = OB_NOT_SUPPORTED;
            LOG_WARN("unsupported file table column type",
                     K(ret), K(expr->datum_meta_.type_));
            break;
          }
          }
        }
        if (OB_SUCC(ret)) {
          expr->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
