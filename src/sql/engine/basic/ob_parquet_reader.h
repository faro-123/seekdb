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

#ifndef _OB_PARQUET_READER_H
#define _OB_PARQUET_READER_H

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

// reads a local parquet file, only the flat schema is supported
class ObParquetReader
{
public:
  ObParquetReader();
  ~ObParquetReader();

  int init(common::ObIAllocator &alloc, const common::ObString &path);
  int get_schema(common::ObIArray<FileColumnMeta> &columns);
  // only decode the given file columns, all the columns are decoded if it is empty
  int set_projection(const common::ObIArray<int64_t> &col_idxs);
  int next_row(common::ObIArray<FileCell> &row);
  int64_t row_count() const { return row_count_; }

private:
  struct ColumnSchema
  {
    ColumnSchema() : type_(-1), repetition_(-1), converted_type_(-1), logical_type_(-1),
                     type_length_(0), num_children_(0), name_() {}
    int32_t type_;
    int32_t repetition_;
    int32_t converted_type_;
    int32_t logical_type_;
    int32_t type_length_;
    int32_t num_children_;
    common::ObString name_;
  };

  struct ChunkInfo
  {
    ChunkInfo() : data_page_offset_(0), dictionary_page_offset_(0), num_values_(0),
                  codec_(0), type_(-1) {}
    int64_t data_page_offset_;
    int64_t dictionary_page_offset_;
    int64_t num_values_;
    int32_t codec_;
    int32_t type_;
  };

  struct RowGroupInfo
  {
    RowGroupInfo() : num_rows_(0), chunk_begin_(0), chunk_count_(0) {}
    int64_t num_rows_;
    int64_t chunk_begin_;
    int64_t chunk_count_;
  };

  struct PageHeaderInfo
  {
    PageHeaderInfo() : page_type(0), uncompressed_size(0), compressed_size(0), num_values(0),
                       encoding(0), def_levels_byte_length(0), rep_levels_byte_length(0),
                       num_nulls(0), is_compressed(true), header_size(0) {}
    int32_t page_type;
    int32_t uncompressed_size;
    int32_t compressed_size;
    int32_t num_values;
    int32_t encoding;
    int32_t def_levels_byte_length;
    int32_t rep_levels_byte_length;
    int32_t num_nulls;
    bool is_compressed;
    int64_t header_size;
  };

  int read_file(const common::ObString &path);
  int parse_footer();
  int parse_schema();
  int parse_row_groups();
  int decode_row_groups();
  int decode_column_chunk(int64_t col_idx, int64_t row_group_idx);
  int read_page_header(int64_t offset, PageHeaderInfo &info);
  int decode_plain_values(const char *data, int64_t len, int64_t num_values, int32_t type,
                          const ColumnSchema &element, common::ObArray<FileCell> &values);
  int decode_dict_indices(const char *data, int64_t len, int64_t num_values,
                          const common::ObArray<FileCell> &dict, common::ObArray<FileCell> &values);
  int copy_string(const char *data, int64_t len, common::ObString &out);
  bool is_projected(int64_t col_idx) const;

private:
  common::ObIAllocator *alloc_;
  char *buf_;
  int64_t buf_len_;
  char *footer_;
  int64_t footer_len_;
  common::ObArray<ColumnSchema> schema_;
  common::ObArray<ChunkInfo> chunks_;
  common::ObArray<RowGroupInfo> row_groups_;
  // the values of all the columns, the cell of the column c and the row r is
  // stored in the index c * row_count_ + r
  common::ObArray<FileCell> all_cells_;
  common::ObArray<int64_t> projection_;
  int64_t row_count_;
  int64_t cur_row_;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* _OB_PARQUET_READER_H */
