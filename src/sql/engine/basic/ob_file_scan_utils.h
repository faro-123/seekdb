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

#ifndef _OB_FILE_SCAN_UTILS_H
#define _OB_FILE_SCAN_UTILS_H

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"

namespace oceanbase
{
namespace sql
{

class ObParquetReader;
class ObCsvFileReader;
class ObJsonFileReader;

enum class FileScanFormat : int32_t
{
  INVALID = 0,
  CSV,
  JSON,
  PARQUET,
  GLOB,
};

// column types the file table functions can infer and scan
enum class FileColumnType : int32_t
{
  INVALID = 0,
  BIGINT,
  DOUBLE,
  BOOLEAN,
  VARCHAR,
  DATE,
  DATETIME,
};

struct FileColumnMeta
{
  FileColumnMeta() : name_(), type_(FileColumnType::INVALID), nullable_(true), scale_(0) {}
  common::ObString name_;
  FileColumnType type_;
  bool nullable_;
  // fractional second precision observed for DATETIME columns
  int32_t scale_;
};

// one scanned cell, the concrete member depends on the column type
struct FileCell
{
  FileCell() : is_null_(true), str_(), int_val_(0), dbl_val_(0.0), bool_val_(false) {}
  void set_null() { is_null_ = true; }
  bool is_null_;
  common::ObString str_;
  int64_t int_val_;
  double dbl_val_;
  bool bool_val_;
};

class ObFileScanUtils
{
public:
  // upper bound of a single file which can be loaded into memory
  static const int64_t MAX_FILE_SIZE = 512L * 1024L * 1024L;

  static const char *type_name(FileColumnType type);
  // resolve the file path to an absolute path, the path must exist
  static int get_full_path(const common::ObString &path,
                           common::ObIAllocator &alloc,
                           common::ObString &full_path);
  // reuse the secure-file-priv restriction of the server
  static int check_secure_path(const common::ObString &secure_file_priv,
                               const common::ObString &full_path);
  // infer the schema of the whole file, sample_size must be -1 (all rows)
  static int infer_schema(const common::ObString &full_path,
                           FileScanFormat format,
                           bool has_header,
                           int64_t sample_size,
                           common::ObIAllocator &alloc,
                           common::ObIArray<FileColumnMeta> &columns);
  static int infer_csv_schema(const common::ObString &full_path,
                              bool has_header,
                              common::ObIAllocator &alloc,
                              common::ObIArray<FileColumnMeta> &columns);
  static int infer_json_schema(const common::ObString &full_path,
                               common::ObIAllocator &alloc,
                               common::ObIArray<FileColumnMeta> &columns);
  static int infer_parquet_schema(const common::ObString &full_path,
                                  common::ObIAllocator &alloc,
                                  common::ObIArray<FileColumnMeta> &columns);
  // merge an observed value into the column type, scale tracks the fractional
  // second digits of DATETIME values so the column keeps the file's precision
  static int infer_string_type(const common::ObString &value, FileColumnType &type, int32_t &scale);
  static bool is_true_literal(const common::ObString &value);
  static bool is_false_literal(const common::ObString &value);
  static int parse_int64(const common::ObString &value, int64_t &val);
  static int parse_double(const common::ObString &value, double &val);
  static int parse_bool(const common::ObString &value, bool &val);
  // "YYYY-MM-DD" to days since 1970-01-01
  static int parse_date(const common::ObString &value, int32_t &days);
  // "YYYY-MM-DD[ T]HH:MM:SS[.ffffff]" to microseconds since 1970-01-01,
  // frac_digits receives the number of fractional second digits in the text
  static int parse_datetime(const common::ObString &value, int64_t &micros,
                            int32_t *frac_digits = NULL);
  // convert a raw text value to the target column type
  static int convert_cell(const common::ObString &raw, FileColumnType type, FileCell &cell);
  // read the whole content of a local file
  static int read_whole_file(const common::ObString &full_path,
                             common::ObIAllocator &alloc,
                             char *&buf,
                             int64_t &buf_len);
};

// scans a local file row by row, the schema is fixed by the resolver
class ObFileScanner
{
public:
  ObFileScanner();
  ~ObFileScanner();

  int init(common::ObIAllocator &alloc,
           const common::ObString &full_path,
           FileScanFormat format,
           bool has_header,
           const common::ObIArray<FileColumnMeta> &schema,
           const common::ObIArray<int64_t> *projection = NULL);
  // fill one row, OB_ITER_END is returned when the file is exhausted
  int next_row(common::ObIArray<FileCell> &row);
  int64_t row_count() const { return row_count_; }

private:
  int init_csv();
  int init_json();
  int init_parquet();
  int init_glob();
  int next_csv_row(common::ObIArray<FileCell> &row);
  int next_json_row(common::ObIArray<FileCell> &row);
  int next_glob_row(common::ObIArray<FileCell> &row);
  int check_csv_header();

private:
  common::ObIAllocator *alloc_;
  FileScanFormat format_;
  bool has_header_;
  common::ObString path_;
  common::ObArray<FileColumnMeta> schema_;
  common::ObArray<common::ObString> glob_files_;
  common::ObArray<int64_t> projection_;
  char *buf_;
  int64_t buf_len_;
  int64_t row_count_;
  int64_t glob_idx_;
  ObCsvFileReader *csv_reader_;
  ObJsonFileReader *json_reader_;
  ObParquetReader *parquet_reader_;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* _OB_FILE_SCAN_UTILS_H */
