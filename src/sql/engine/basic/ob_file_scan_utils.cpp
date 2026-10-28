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

#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_parquet_reader.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_array.h"
#include <errno.h>
#include <stdlib.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace oceanbase
{
namespace sql
{

namespace
{

enum JsonValueType : int8_t
{
  JSON_VALUE_NULL = 0,
  JSON_VALUE_STRING,
  JSON_VALUE_INT,
  JSON_VALUE_DOUBLE,
  JSON_VALUE_BOOL,
};

struct JsonKeyValue
{
  JsonKeyValue() : key_(), type_(JSON_VALUE_NULL), str_(), int_val_(0), dbl_val_(0.0), bool_val_(false) {}
  common::ObString key_;
  int8_t type_;
  common::ObString str_;
  int64_t int_val_;
  double dbl_val_;
  bool bool_val_;
};

int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

bool is_leap_year(int64_t y)
{
  return (0 == y % 4 && 0 != y % 100) || 0 == y % 400;
}

int days_in_month(int64_t y, int64_t m)
{
  static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) {
    return 0;
  }
  if (2 == m && is_leap_year(y)) {
    return 29;
  }
  return days[m - 1];
}

bool is_space_char(char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c;
}

// trim the leading and trailing spaces of a text value
void trim_value(const char *data, int64_t len, int64_t &begin, int64_t &end)
{
  while (begin < end && is_space_char(data[begin])) {
    ++begin;
  }
  while (end > begin && is_space_char(data[end - 1])) {
    --end;
  }
}

bool match_pattern(const char *pattern, const char *name)
{
  if (NULL == pattern || NULL == name) {
    return false;
  }
  const char *star = NULL;
  const char *match = NULL;
  while (*name != '\0') {
    if ('*' == *pattern) {
      star = pattern++;
      match = name;
    } else if ('?' == *pattern || *pattern == *name) {
      ++pattern;
      ++name;
    } else if (NULL != star) {
      pattern = star + 1;
      name = ++match;
    } else {
      return false;
    }
  }
  while ('*' == *pattern) {
    ++pattern;
  }
  return '\0' == *pattern;
}

int compare_string(const common::ObString &lhs, const common::ObString &rhs)
{
  const int64_t len = lhs.length() < rhs.length() ? lhs.length() : rhs.length();
  int ret = 0;
  if (len > 0) {
    ret = MEMCMP(lhs.ptr(), rhs.ptr(), len);
  }
  if (0 == ret) {
    ret = (lhs.length() == rhs.length()) ? 0 : (lhs.length() < rhs.length() ? -1 : 1);
  }
  return ret;
}

int to_c_string(const common::ObString &str, char *buf, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(str.length() >= buf_len)) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    if (str.length() > 0) {
      MEMCPY(buf, str.ptr(), str.length());
    }
    buf[str.length()] = '\0';
  }
  return ret;
}

} // namespace

/*
 * csv reader
 */
class ObCsvFileReader
{
public:
  ObCsvFileReader() : alloc_(NULL), buf_(NULL), buf_len_(0), pos_(0),
                      scratch_(NULL), scratch_cap_(0), scratch_used_(0) {}
  ~ObCsvFileReader() {}

  int init(common::ObIAllocator &alloc, const char *buf, int64_t buf_len)
  {
    alloc_ = &alloc;
    buf_ = buf;
    buf_len_ = buf_len;
    pos_ = 0;
    scratch_ = static_cast<char *>(alloc.alloc(1024));
    if (OB_ISNULL(scratch_)) {
      return OB_ALLOCATE_MEMORY_FAILED;
    }
    scratch_cap_ = 1024;
    scratch_used_ = 0;
    return OB_SUCCESS;
  }

  int next_record(common::ObIArray<common::ObString> &fields)
  {
    int ret = OB_SUCCESS;
    fields.reuse();
    scratch_used_ = 0;
    if (pos_ >= buf_len_) {
      ret = OB_ITER_END;
    } else {
      bool record_done = false;
      while (OB_SUCC(ret) && !record_done) {
        common::ObString value;
        if (OB_FAIL(next_field(value))) {
        } else if (OB_FAIL(fields.push_back(value))) {
        } else if (pos_ < buf_len_ && ',' == buf_[pos_]) {
          ++pos_;
        } else {
          // end of the record
          if (pos_ < buf_len_) {
            if ('\r' == buf_[pos_]) {
              ++pos_;
              if (pos_ < buf_len_ && '\n' == buf_[pos_]) {
                ++pos_;
              }
            } else if ('\n' == buf_[pos_]) {
              ++pos_;
            }
          }
          record_done = true;
        }
      }
    }
    return ret;
  }

private:
  int next_field(common::ObString &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ < buf_len_ && '"' == buf_[pos_]) {
      ++pos_;
      const int64_t scratch_start = scratch_used_;
      while (pos_ < buf_len_) {
        const char c = buf_[pos_];
        if ('"' == c) {
          if (pos_ + 1 < buf_len_ && '"' == buf_[pos_ + 1]) {
            if (OB_FAIL(append_scratch("\"", 1))) {
            } else {
              pos_ += 2;
            }
          } else {
            ++pos_;
            break;
          }
        } else if ('\r' == c) {
          if (pos_ + 1 < buf_len_ && '\n' == buf_[pos_ + 1]) {
            if (OB_FAIL(append_scratch("\n", 1))) {
            } else {
              pos_ += 2;
            }
          } else if (OB_FAIL(append_scratch("\r", 1))) {
          } else {
            ++pos_;
          }
        } else if (OB_FAIL(append_scratch(&c, 1))) {
        } else {
          ++pos_;
        }
      }
      value = common::ObString(scratch_used_ - scratch_start, scratch_ + scratch_start);
    } else {
      const int64_t begin = pos_;
      while (pos_ < buf_len_) {
        const char c = buf_[pos_];
        if (',' == c || '\n' == c || '\r' == c) {
          break;
        }
        ++pos_;
      }
      value = common::ObString(pos_ - begin, buf_ + begin);
    }
    return ret;
  }

  int append_scratch(const char *data, int64_t len)
  {
    int ret = OB_SUCCESS;
    if (scratch_used_ + len > scratch_cap_) {
      int64_t new_cap = scratch_cap_ > 0 ? scratch_cap_ : 4096;
      while (new_cap < scratch_used_ + len) {
        new_cap *= 2;
      }
      char *new_buf = static_cast<char *>(alloc_->alloc(new_cap));
      if (OB_ISNULL(new_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (scratch_used_ > 0) {
          MEMCPY(new_buf, scratch_, scratch_used_);
        }
        scratch_ = new_buf;
        scratch_cap_ = new_cap;
      }
    }
    if (OB_SUCC(ret)) {
      MEMCPY(scratch_ + scratch_used_, data, len);
      scratch_used_ += len;
    }
    return ret;
  }

private:
  common::ObIAllocator *alloc_;
  const char *buf_;
  int64_t buf_len_;
  int64_t pos_;
  char *scratch_;
  int64_t scratch_cap_;
  int64_t scratch_used_;
};

/*
 * json lines reader
 */
class ObJsonFileReader
{
public:
  ObJsonFileReader() : alloc_(NULL), buf_(NULL), buf_len_(0), pos_(0),
                       scratch_(NULL), scratch_cap_(0), scratch_used_(0) {}
  ~ObJsonFileReader() {}

  int init(common::ObIAllocator &alloc, const char *buf, int64_t buf_len)
  {
    alloc_ = &alloc;
    buf_ = buf;
    buf_len_ = buf_len;
    pos_ = 0;
    scratch_ = static_cast<char *>(alloc.alloc(1024));
    if (OB_ISNULL(scratch_)) {
      return OB_ALLOCATE_MEMORY_FAILED;
    }
    scratch_cap_ = 1024;
    scratch_used_ = 0;
    return OB_SUCCESS;
  }

  // parse the next json object of the file
  int next_record(common::ObIArray<JsonKeyValue> &kvs)
  {
    int ret = OB_SUCCESS;
    kvs.reuse();
    scratch_used_ = 0;
    while (OB_SUCC(ret)) {
      // skip the empty lines
      while (pos_ < buf_len_ && ('\n' == buf_[pos_] || '\r' == buf_[pos_] || ' ' == buf_[pos_] || '\t' == buf_[pos_])) {
        ++pos_;
      }
      if (pos_ >= buf_len_) {
        ret = OB_ITER_END;
        break;
      }
      const int64_t line_begin = pos_;
      while (pos_ < buf_len_ && '\n' != buf_[pos_]) {
        ++pos_;
      }
      const int64_t line_end = pos_;
      if (pos_ < buf_len_) {
        ++pos_;
      }
      if (line_end > line_begin) {
        if (OB_FAIL(parse_object(buf_ + line_begin, line_end - line_begin, kvs))) {
          LOG_WARN("failed to parse the json line", K(line_begin));
        }
        break;
      }
    }
    return ret;
  }

private:
  int parse_object(const char *data, int64_t len, common::ObIArray<JsonKeyValue> &kvs)
  {
    int ret = OB_SUCCESS;
    int64_t pos = 0;
    skip_space(data, len, pos);
    if (pos >= len || '{' != data[pos]) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ++pos;
      bool object_done = false;
      while (OB_SUCC(ret) && !object_done) {
        skip_space(data, len, pos);
        if (pos < len && '}' == data[pos]) {
          ++pos;
          object_done = true;
        } else {
          JsonKeyValue kv;
          if (OB_FAIL(parse_string(data, len, pos, kv.key_))) {
          } else {
            skip_space(data, len, pos);
            if (pos >= len || ':' != data[pos]) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              ++pos;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(parse_value(data, len, pos, kv))) {
          } else if (OB_FAIL(kvs.push_back(kv))) {
          } else {
            skip_space(data, len, pos);
            if (pos < len && ',' == data[pos]) {
              ++pos;
            } else if (pos < len && '}' == data[pos]) {
              ++pos;
              object_done = true;
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      skip_space(data, len, pos);
      if (pos != len) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    return ret;
  }

  int parse_value(const char *data, int64_t len, int64_t &pos, JsonKeyValue &kv)
  {
    int ret = OB_SUCCESS;
    skip_space(data, len, pos);
    if (pos >= len) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const char c = data[pos];
      if ('"' == c) {
        kv.type_ = JSON_VALUE_STRING;
        ret = parse_string(data, len, pos, kv.str_);
      } else if ('{' == c || '[' == c) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested json objects and arrays");
      } else if ('t' == c) {
        if (pos + 4 <= len && 0 == MEMCMP(data + pos, "true", 4)) {
          pos += 4;
          kv.type_ = JSON_VALUE_BOOL;
          kv.bool_val_ = true;
        } else {
          ret = OB_ERR_UNEXPECTED;
        }
      } else if ('f' == c) {
        if (pos + 5 <= len && 0 == MEMCMP(data + pos, "false", 5)) {
          pos += 5;
          kv.type_ = JSON_VALUE_BOOL;
          kv.bool_val_ = false;
        } else {
          ret = OB_ERR_UNEXPECTED;
        }
      } else if ('n' == c) {
        if (pos + 4 <= len && 0 == MEMCMP(data + pos, "null", 4)) {
          pos += 4;
          kv.type_ = JSON_VALUE_NULL;
        } else {
          ret = OB_ERR_UNEXPECTED;
        }
      } else {
        ret = parse_number(data, len, pos, kv);
      }
    }
    return ret;
  }

  int parse_number(const char *data, int64_t len, int64_t &pos, JsonKeyValue &kv)
  {
    int ret = OB_SUCCESS;
    const int64_t begin = pos;
    bool is_double = false;
    if (pos < len && ('-' == data[pos] || '+' == data[pos])) {
      ++pos;
    }
    while (pos < len) {
      const char c = data[pos];
      if (c >= '0' && c <= '9') {
        ++pos;
      } else if ('.' == c || 'e' == c || 'E' == c) {
        is_double = true;
        ++pos;
      } else {
        break;
      }
    }
    if (pos == begin) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      common::ObString text(pos - begin, data + begin);
      if (is_double) {
        kv.type_ = JSON_VALUE_DOUBLE;
        if (OB_FAIL(ObFileScanUtils::parse_double(text, kv.dbl_val_))) {
          ret = OB_ERR_UNEXPECTED;
        }
      } else {
        kv.type_ = JSON_VALUE_INT;
        if (OB_FAIL(ObFileScanUtils::parse_int64(text, kv.int_val_))) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
    return ret;
  }

  int parse_string(const char *data, int64_t len, int64_t &pos, common::ObString &value)
  {
    int ret = OB_SUCCESS;
    if (pos >= len || '"' != data[pos]) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ++pos;
      const int64_t scratch_start = scratch_used_;
      bool closed = false;
      while (pos < len) {
        const char c = data[pos];
        if ('"' == c) {
          ++pos;
          closed = true;
          break;
        } else if ('\\' == c) {
          ++pos;
          if (pos >= len) {
            break;
          }
          const char esc = data[pos];
          switch (esc) {
          case '"': if (OB_FAIL(append_scratch("\"", 1))) { } else { ++pos; } break;
          case '\\': if (OB_FAIL(append_scratch("\\", 1))) { } else { ++pos; } break;
          case '/': if (OB_FAIL(append_scratch("/", 1))) { } else { ++pos; } break;
          case 'b': if (OB_FAIL(append_scratch("\b", 1))) { } else { ++pos; } break;
          case 'f': if (OB_FAIL(append_scratch("\f", 1))) { } else { ++pos; } break;
          case 'n': if (OB_FAIL(append_scratch("\n", 1))) { } else { ++pos; } break;
          case 'r': if (OB_FAIL(append_scratch("\r", 1))) { } else { ++pos; } break;
          case 't': if (OB_FAIL(append_scratch("\t", 1))) { } else { ++pos; } break;
          case 'u': {
            uint32_t code = 0;
            if (OB_FAIL(parse_unicode(data, len, pos, code))) {
            } else if (OB_FAIL(append_utf8(code))) {
            }
            break;
          }
          default:
            ret = OB_ERR_UNEXPECTED;
            break;
          }
        } else if (OB_FAIL(append_scratch(&c, 1))) {
        } else {
          ++pos;
        }
      }
      if (OB_SUCC(ret) && !closed) {
        ret = OB_ERR_UNEXPECTED;
      }
      if (OB_SUCC(ret)) {
        value = common::ObString(scratch_used_ - scratch_start, scratch_ + scratch_start);
      }
    }
    return ret;
  }

  int parse_unicode(const char *data, int64_t len, int64_t &pos, uint32_t &code)
  {
    int ret = OB_SUCCESS;
    if (pos + 4 > len) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      code = 0;
      for (int64_t i = 1; i <= 4; ++i) {
        const char c = data[pos + i];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
          digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
          digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
          digit = c - 'A' + 10;
        } else {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        code = (code << 4) | digit;
      }
      if (OB_SUCC(ret)) {
        pos += 4;
        if (code >= 0xD800 && code <= 0xDBFF && pos + 6 <= len
            && '\\' == data[pos] && 'u' == data[pos + 1]) {
          uint32_t low = 0;
          const int64_t saved_pos = pos;
          pos += 2;
          if (OB_SUCC(parse_unicode(data, len, pos, low)) && low >= 0xDC00 && low <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          } else {
            pos = saved_pos;
          }
        }
      }
    }
    return ret;
  }

  int append_utf8(uint32_t code)
  {
    int ret = OB_SUCCESS;
    char buf[4];
    int64_t len = 0;
    if (code < 0x80) {
      buf[0] = static_cast<char>(code);
      len = 1;
    } else if (code < 0x800) {
      buf[0] = static_cast<char>(0xC0 | (code >> 6));
      buf[1] = static_cast<char>(0x80 | (code & 0x3F));
      len = 2;
    } else if (code < 0x10000) {
      buf[0] = static_cast<char>(0xE0 | (code >> 12));
      buf[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      buf[2] = static_cast<char>(0x80 | (code & 0x3F));
      len = 3;
    } else {
      buf[0] = static_cast<char>(0xF0 | (code >> 18));
      buf[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      buf[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      buf[3] = static_cast<char>(0x80 | (code & 0x3F));
      len = 4;
    }
    ret = append_scratch(buf, len);
    return ret;
  }

  int append_scratch(const char *data, int64_t len)
  {
    int ret = OB_SUCCESS;
    if (scratch_used_ + len > scratch_cap_) {
      int64_t new_cap = scratch_cap_ > 0 ? scratch_cap_ : 4096;
      while (new_cap < scratch_used_ + len) {
        new_cap *= 2;
      }
      char *new_buf = static_cast<char *>(alloc_->alloc(new_cap));
      if (OB_ISNULL(new_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (scratch_used_ > 0) {
          MEMCPY(new_buf, scratch_, scratch_used_);
        }
        scratch_ = new_buf;
        scratch_cap_ = new_cap;
      }
    }
    if (OB_SUCC(ret)) {
      MEMCPY(scratch_ + scratch_used_, data, len);
      scratch_used_ += len;
    }
    return ret;
  }

  static void skip_space(const char *data, int64_t len, int64_t &pos)
  {
    while (pos < len && is_space_char(data[pos])) {
      ++pos;
    }
  }

private:
  common::ObIAllocator *alloc_;
  const char *buf_;
  int64_t buf_len_;
  int64_t pos_;
  char *scratch_;
  int64_t scratch_cap_;
  int64_t scratch_used_;
};

namespace {

// the duplicated header names are renamed with a "__N" suffix
int build_csv_column_names(common::ObIAllocator &alloc,
                           const common::ObIArray<common::ObString> &fields,
                           common::ObIArray<common::ObString> &names)
{
  int ret = OB_SUCCESS;
  names.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
    const common::ObString &field = fields.at(i);
    int64_t same_count = 0;
    for (int64_t j = 0; j < i; ++j) {
      if (0 == compare_string(fields.at(j), field)) {
        ++same_count;
      }
    }
    // the name must be copied, the field may point to the reader's scratch buffer
    char new_name_buf[OB_MAX_COLUMN_NAME_LENGTH + 32];
    const int64_t name_len = field.length();
    int64_t new_name_len = name_len;
    if (OB_UNLIKELY(name_len + 32 >= static_cast<int64_t>(sizeof(new_name_buf)))) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      MEMCPY(new_name_buf, field.ptr(), name_len);
      if (same_count > 0) {
        new_name_len += snprintf(new_name_buf + name_len, sizeof(new_name_buf) - name_len,
                                 "__%ld", same_count + 1);
      }
      common::ObString name;
      if (OB_FAIL(ob_write_string(alloc, common::ObString(new_name_len, new_name_buf), name))) {
      } else if (OB_FAIL(names.push_back(name))) {
      }
    }
  }
  return ret;
}

} // namespace

const char *ObFileScanUtils::type_name(FileColumnType type)
{
  switch (type) {
  case FileColumnType::BIGINT: return "BIGINT";
  case FileColumnType::DOUBLE: return "DOUBLE";
  case FileColumnType::BOOLEAN: return "BOOLEAN";
  case FileColumnType::VARCHAR: return "VARCHAR";
  case FileColumnType::DATE: return "DATE";
  case FileColumnType::DATETIME: return "DATETIME";
  default: return "VARCHAR";
  }
}

int ObFileScanUtils::get_full_path(const common::ObString &path,
                                   common::ObIAllocator &alloc,
                                   common::ObString &full_path)
{
  int ret = OB_SUCCESS;
  full_path.reset();
  if (path.empty()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "empty file path");
  } else {
    char path_buf[PATH_MAX + 1];
    char buf[PATH_MAX + 1];
    MEMSET(path_buf, 0, sizeof(path_buf));
    MEMSET(buf, 0, sizeof(buf));
    char *resolved = NULL;
    if (OB_FAIL(to_c_string(path, path_buf, sizeof(path_buf)))) {
    } else {
#ifdef _WIN32
      resolved = _fullpath(buf, path_buf, PATH_MAX);
#else
      resolved = realpath(path_buf, buf);
#endif
    }
    if (OB_FAIL(ret)) {
    } else
    if (NULL == resolved) {
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST);
    } else if (OB_FAIL(ob_write_string(alloc, common::ObString(resolved), full_path))) {
    } else { /* do nothing */ }
  }
  return ret;
}

int ObFileScanUtils::check_secure_path(const common::ObString &secure_file_priv,
                                       const common::ObString &full_path)
{
  return ObResolverUtils::check_secure_path(secure_file_priv, full_path);
}

int ObFileScanUtils::read_whole_file(const common::ObString &full_path,
                                     common::ObIAllocator &alloc,
                                     char *&buf,
                                     int64_t &buf_len)
{
  int ret = OB_SUCCESS;
  buf = NULL;
  buf_len = 0;
  char path_buf[PATH_MAX + 1];
  FILE *fp = NULL;
  if (OB_FAIL(to_c_string(full_path, path_buf, sizeof(path_buf)))) {
  } else if (NULL == (fp = fopen(path_buf, "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST);
  } else {
    if (0 != fseek(fp, 0, SEEK_END)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const long file_size = ftell(fp);
      if (file_size < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else if (file_size > MAX_FILE_SIZE) {
        ret = OB_SIZE_OVERFLOW;
        LOG_USER_ERROR(OB_SIZE_OVERFLOW);
      } else {
        buf_len = static_cast<int64_t>(file_size);
        if (0 != fseek(fp, 0, SEEK_SET)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(buf_len + 1)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (buf_len > 0 && buf_len != static_cast<int64_t>(fread(buf, 1, buf_len, fp))) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          buf[buf_len] = '\0';
        }
      }
    }
    fclose(fp);
  }
  return ret;
}

bool ObFileScanUtils::is_true_literal(const common::ObString &value)
{
  return 4 == value.length() && 0 == value.case_compare("true");
}

bool ObFileScanUtils::is_false_literal(const common::ObString &value)
{
  return 5 == value.length() && 0 == value.case_compare("false");
}

int ObFileScanUtils::parse_int64(const common::ObString &value, int64_t &val)
{
  int ret = OB_SUCCESS;
  int64_t begin = 0;
  int64_t end = value.length();
  trim_value(value.ptr(), value.length(), begin, end);
  if (begin >= end) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    bool valid = true;
    int64_t i = begin;
    bool negative = false;
    if ('+' == value.ptr()[i] || '-' == value.ptr()[i]) {
      negative = ('-' == value.ptr()[i]);
      ++i;
    }
    if (i >= end) {
      valid = false;
    }
    int64_t result = 0;
    for (; i < end && valid; ++i) {
      const char c = value.ptr()[i];
      if (c < '0' || c > '9') {
        valid = false;
      } else {
        result = result * 10 + (c - '0');
        if (result > 0x7FFFFFFFFFFFFFFFLL) {
          valid = false;
        }
      }
    }
    if (!valid) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      val = negative ? -result : result;
    }
  }
  return ret;
}

int ObFileScanUtils::parse_double(const common::ObString &value, double &val)
{
  int ret = OB_SUCCESS;
  int64_t begin = 0;
  int64_t end = value.length();
  trim_value(value.ptr(), value.length(), begin, end);
  if (begin >= end) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    char buf[128];
    const int64_t len = end - begin;
    if (len >= static_cast<int64_t>(sizeof(buf))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      MEMCPY(buf, value.ptr() + begin, len);
      buf[len] = '\0';
      char *endptr = NULL;
      errno = 0;
      const double result = strtod(buf, &endptr);
      if (NULL == endptr || endptr != buf + len || 0 != errno) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        val = result;
      }
    }
  }
  return ret;
}

int ObFileScanUtils::parse_bool(const common::ObString &value, bool &val)
{
  int ret = OB_SUCCESS;
  if (is_true_literal(value)) {
    val = true;
  } else if (is_false_literal(value)) {
    val = false;
  } else {
    int64_t num = 0;
    if (OB_SUCC(parse_int64(value, num)) && (0 == num || 1 == num)) {
      val = (1 == num);
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObFileScanUtils::parse_date(const common::ObString &value, int32_t &days)
{
  int ret = OB_SUCCESS;
  int64_t begin = 0;
  int64_t end = value.length();
  trim_value(value.ptr(), value.length(), begin, end);
  if (end - begin != 10 || '-' != value.ptr()[begin + 4] || '-' != value.ptr()[begin + 7]) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    bool valid = true;
    for (int64_t i = 0; i < 4 && valid; ++i) {
      const char c = value.ptr()[begin + i];
      if (c < '0' || c > '9') {
        valid = false;
      } else {
        year = year * 10 + (c - '0');
      }
    }
    for (int64_t i = 5; i < 7 && valid; ++i) {
      const char c = value.ptr()[begin + i];
      if (c < '0' || c > '9') {
        valid = false;
      } else {
        month = month * 10 + (c - '0');
      }
    }
    for (int64_t i = 8; i < 10 && valid; ++i) {
      const char c = value.ptr()[begin + i];
      if (c < '0' || c > '9') {
        valid = false;
      } else {
        day = day * 10 + (c - '0');
      }
    }
    if (!valid || year < 1 || month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      days = static_cast<int32_t>(days_from_civil(year, month, day));
    }
  }
  return ret;
}

int ObFileScanUtils::parse_datetime(const common::ObString &value, int64_t &micros)
{
  int ret = OB_SUCCESS;
  int64_t begin = 0;
  int64_t end = value.length();
  trim_value(value.ptr(), value.length(), begin, end);
  int32_t days = 0;
  if (end - begin < 10 || OB_FAIL(parse_date(common::ObString(10, value.ptr() + begin), days))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t pos = begin + 10;
    int64_t hour = 0;
    int64_t minute = 0;
    int64_t second = 0;
    int64_t fraction = 0;
    bool valid = true;
    if (pos < end && (' ' == value.ptr()[pos] || 'T' == value.ptr()[pos])) {
      ++pos;
      if (pos + 2 > end) {
        valid = false;
      } else {
        hour = (value.ptr()[pos] - '0') * 10 + (value.ptr()[pos + 1] - '0');
        pos += 2;
        if (pos + 2 >= end || ':' != value.ptr()[pos]) {
          valid = false;
        } else {
          ++pos;
          minute = (value.ptr()[pos] - '0') * 10 + (value.ptr()[pos + 1] - '0');
          pos += 2;
          if (pos < end && ':' == value.ptr()[pos]) {
            ++pos;
            if (pos + 2 > end) {
              valid = false;
            } else {
              second = (value.ptr()[pos] - '0') * 10 + (value.ptr()[pos + 1] - '0');
              pos += 2;
              if (pos < end && '.' == value.ptr()[pos]) {
                ++pos;
                int64_t digits = 0;
                while (pos < end && value.ptr()[pos] >= '0' && value.ptr()[pos] <= '9') {
                  if (digits < 6) {
                    fraction = fraction * 10 + (value.ptr()[pos] - '0');
                    ++digits;
                  }
                  ++pos;
                }
                while (digits < 6) {
                  fraction *= 10;
                  ++digits;
                }
              }
            }
          }
        }
      }
    }
    if (!valid || hour > 23 || minute > 59 || second > 59) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      micros = static_cast<int64_t>(days) * 24 * 3600 * 1000000
               + hour * 3600 * 1000000 + minute * 60 * 1000000 + second * 1000000 + fraction;
    }
  }
  return ret;
}

int ObFileScanUtils::infer_string_type(const common::ObString &value, FileColumnType &type)
{
  int ret = OB_SUCCESS;
  FileColumnType observed = FileColumnType::VARCHAR;
  if (value.empty()) {
    // an empty text is not a value
  } else if (is_true_literal(value) || is_false_literal(value)) {
    observed = FileColumnType::BOOLEAN;
  } else {
    int64_t int_val = 0;
    double dbl_val = 0.0;
    int32_t days = 0;
    int64_t micros = 0;
    int probe_ret = parse_int64(value, int_val);
    if (OB_SUCCESS == probe_ret) {
      observed = FileColumnType::BIGINT;
    } else if (OB_SUCCESS == (probe_ret = parse_double(value, dbl_val))) {
      observed = FileColumnType::DOUBLE;
    } else if (OB_SUCCESS == (probe_ret = parse_date(value, days))) {
      observed = FileColumnType::DATE;
    } else if (OB_SUCCESS == (probe_ret = parse_datetime(value, micros))) {
      observed = FileColumnType::DATETIME;
    } else {
      observed = FileColumnType::VARCHAR;
    }
  }
  if (FileColumnType::INVALID == type) {
    type = observed;
  } else if (type != observed) {
    if ((FileColumnType::BIGINT == type && FileColumnType::DOUBLE == observed)
        || (FileColumnType::DOUBLE == type && FileColumnType::BIGINT == observed)) {
      type = FileColumnType::DOUBLE;
    } else if (FileColumnType::DATE == type && FileColumnType::DATETIME == observed) {
      type = FileColumnType::DATETIME;
    } else if (FileColumnType::DATETIME == type && FileColumnType::DATE == observed) {
      type = FileColumnType::DATETIME;
    } else {
      type = FileColumnType::VARCHAR;
    }
  }
  return ret;
}

int ObFileScanUtils::convert_cell(const common::ObString &raw, FileColumnType type, FileCell &cell)
{
  int ret = OB_SUCCESS;
  cell = FileCell();
  if (raw.empty()) {
    cell.set_null();
  } else {
    switch (type) {
    case FileColumnType::BIGINT: {
      int64_t val = 0;
      if (OB_SUCC(parse_int64(raw, val))) {
        cell.is_null_ = false;
        cell.int_val_ = val;
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case FileColumnType::DOUBLE: {
      double val = 0.0;
      if (OB_SUCC(parse_double(raw, val))) {
        cell.is_null_ = false;
        cell.dbl_val_ = val;
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case FileColumnType::BOOLEAN: {
      bool val = false;
      if (OB_SUCC(parse_bool(raw, val))) {
        cell.is_null_ = false;
        cell.bool_val_ = val;
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case FileColumnType::DATE: {
      int32_t days = 0;
      if (OB_SUCC(parse_date(raw, days))) {
        cell.is_null_ = false;
        cell.int_val_ = days;
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case FileColumnType::DATETIME: {
      int64_t micros = 0;
      if (OB_SUCC(parse_datetime(raw, micros))) {
        cell.is_null_ = false;
        cell.int_val_ = micros;
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    default: {
      cell.is_null_ = false;
      cell.str_ = raw;
      break;
    }
    }
  }
  return ret;
}

int ObFileScanUtils::infer_schema(const common::ObString &full_path,
                                  FileScanFormat format,
                                  bool has_header,
                                  int64_t sample_size,
                                  common::ObIAllocator &alloc,
                                  common::ObIArray<FileColumnMeta> &columns)
{
  int ret = OB_SUCCESS;
  columns.reuse();
  if (-1 != sample_size) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size, only -1 is supported");
  } else if (FileScanFormat::GLOB == format) {
    FileColumnMeta col;
    col.name_ = common::ObString(4, "file");
    col.type_ = FileColumnType::VARCHAR;
    col.nullable_ = false;
    if (OB_FAIL(columns.push_back(col))) {
    } else { /* do nothing */ }
  } else if (FileScanFormat::CSV == format) {
    ret = infer_csv_schema(full_path, has_header, alloc, columns);
  } else if (FileScanFormat::JSON == format) {
    ret = infer_json_schema(full_path, alloc, columns);
  } else if (FileScanFormat::PARQUET == format) {
    ret = infer_parquet_schema(full_path, alloc, columns);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "the file format");
  }
  return ret;
}

int ObFileScanUtils::infer_csv_schema(const common::ObString &full_path,
                                      bool has_header,
                                      common::ObIAllocator &alloc,
                                      common::ObIArray<FileColumnMeta> &columns)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t buf_len = 0;
  if (OB_FAIL(read_whole_file(full_path, alloc, buf, buf_len))) {
  } else {
    ObCsvFileReader reader;
    common::ObSEArray<common::ObString, 16> fields;
    common::ObArray<FileColumnMeta> meta;
    common::ObArray<bool> nullables;
    if (OB_FAIL(reader.init(alloc, buf, buf_len))) {
    } else if (has_header) {
      if (OB_FAIL(reader.next_record(fields))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file has no header");
      } else {
        common::ObArray<common::ObString> names;
        if (OB_FAIL(build_csv_column_names(alloc, fields, names))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
          FileColumnMeta col;
          col.name_ = names.at(i);
          col.type_ = FileColumnType::INVALID;
          col.nullable_ = false;
          if (OB_FAIL(meta.push_back(col))) {
          } else if (OB_FAIL(nullables.push_back(false))) {
          }
        }
      }
    } else {
      // generate the column names from the first record
      if (OB_FAIL(reader.next_record(fields))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file is empty");
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
          FileColumnMeta col;
          char name[32];
          const int64_t len = snprintf(name, sizeof(name), "column%ld", i + 1);
          col.name_ = common::ObString(len, name);
          col.type_ = FileColumnType::INVALID;
          col.nullable_ = false;
          if (OB_FAIL(meta.push_back(col))) {
          } else if (OB_FAIL(nullables.push_back(false))) {
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < meta.count() && i < fields.count(); ++i) {
          const common::ObString &value = fields.at(i);
          if (value.empty()) {
            nullables.at(i) = true;
          } else if (OB_FAIL(infer_string_type(value, meta.at(i).type_))) {
          }
        }
      }
    }
    // scan all the records to infer the column types
    int64_t record_cnt = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(reader.next_record(fields))) {
        LOG_DEBUG("csv scan finished", K(ret), K(record_cnt));
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
        break;
      }
      ++record_cnt;
      for (int64_t i = 0; OB_SUCC(ret) && i < meta.count() && i < fields.count(); ++i) {
        const common::ObString &value = fields.at(i);
        if (value.empty()) {
          nullables.at(i) = true;
        } else if (OB_FAIL(infer_string_type(value, meta.at(i).type_))) {
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < meta.count(); ++i) {
      if (FileColumnType::INVALID == meta.at(i).type_) {
        meta.at(i).type_ = FileColumnType::VARCHAR;
      }
      meta.at(i).nullable_ = nullables.at(i);
      if (OB_FAIL(columns.push_back(meta.at(i)))) {
      }
    }
  }
  return ret;
}

int ObFileScanUtils::infer_json_schema(const common::ObString &full_path,
                                       common::ObIAllocator &alloc,
                                       common::ObIArray<FileColumnMeta> &columns)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t buf_len = 0;
  if (OB_FAIL(read_whole_file(full_path, alloc, buf, buf_len))) {
  } else {
    ObJsonFileReader reader;
    common::ObSEArray<JsonKeyValue, 16> kvs;
    common::ObArray<FileColumnMeta> meta;
    if (OB_FAIL(reader.init(alloc, buf, buf_len))) {
    } else {
      while (OB_SUCC(ret)) {
        if (OB_FAIL(reader.next_record(kvs))) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
          }
          break;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < kvs.count(); ++i) {
          const JsonKeyValue &kv = kvs.at(i);
          int64_t idx = -1;
          for (int64_t j = 0; j < meta.count(); ++j) {
            if (0 == compare_string(meta.at(j).name_, kv.key_)) {
              idx = j;
              break;
            }
          }
          if (idx < 0) {
            FileColumnMeta col;
            col.type_ = FileColumnType::INVALID;
            col.nullable_ = true;
            if (OB_FAIL(ob_write_string(alloc, kv.key_, col.name_))) {
              break;
            } else if (OB_FAIL(meta.push_back(col))) {
              break;
            }
            idx = meta.count() - 1;
          }
          FileColumnMeta &col = meta.at(idx);
          if (JSON_VALUE_NULL == kv.type_) {
            col.nullable_ = true;
          } else {
            FileColumnType observed = FileColumnType::VARCHAR;
            if (JSON_VALUE_BOOL == kv.type_) {
              observed = FileColumnType::BOOLEAN;
            } else if (JSON_VALUE_INT == kv.type_) {
              observed = FileColumnType::BIGINT;
            } else if (JSON_VALUE_DOUBLE == kv.type_) {
              observed = FileColumnType::DOUBLE;
            } else {
              observed = FileColumnType::INVALID;
              if (OB_FAIL(infer_string_type(kv.str_, observed))) {
                break;
              }
            }
            if (FileColumnType::INVALID == col.type_) {
              col.type_ = observed;
            } else if (col.type_ != observed) {
              if ((FileColumnType::BIGINT == col.type_ && FileColumnType::DOUBLE == observed)
                  || (FileColumnType::DOUBLE == col.type_ && FileColumnType::BIGINT == observed)) {
                col.type_ = FileColumnType::DOUBLE;
              } else if (FileColumnType::DATE == col.type_ && FileColumnType::DATETIME == observed) {
                col.type_ = FileColumnType::DATETIME;
              } else if (FileColumnType::DATETIME == col.type_ && FileColumnType::DATE == observed) {
                col.type_ = FileColumnType::DATETIME;
              } else {
                col.type_ = FileColumnType::VARCHAR;
              }
            }
          }
        }
        // the columns which are absent in this record are nullable
        for (int64_t i = 0; OB_SUCC(ret) && i < meta.count(); ++i) {
          bool found = false;
          for (int64_t j = 0; j < kvs.count(); ++j) {
            if (0 == compare_string(meta.at(i).name_, kvs.at(j).key_)) {
              found = true;
              break;
            }
          }
          if (!found) {
            meta.at(i).nullable_ = true;
          }
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < meta.count(); ++i) {
        if (FileColumnType::INVALID == meta.at(i).type_) {
          meta.at(i).type_ = FileColumnType::VARCHAR;
        }
        if (OB_FAIL(columns.push_back(meta.at(i)))) {
        }
      }
    }
  }
  return ret;
}

int ObFileScanUtils::infer_parquet_schema(const common::ObString &full_path,
                                          common::ObIAllocator &alloc,
                                          common::ObIArray<FileColumnMeta> &columns)
{
  int ret = OB_SUCCESS;
  ObParquetReader reader;
  if (OB_FAIL(reader.init(alloc, full_path))) {
  } else if (OB_FAIL(reader.get_schema(columns))) {
  } else { /* do nothing */ }
  return ret;
}

ObFileScanner::ObFileScanner()
  : alloc_(NULL), format_(FileScanFormat::INVALID), has_header_(true), path_(), schema_(), glob_files_(),
    projection_(), buf_(NULL), buf_len_(0), row_count_(0), glob_idx_(0),
    csv_reader_(NULL), json_reader_(NULL), parquet_reader_(NULL)
{
}

ObFileScanner::~ObFileScanner()
{
  if (OB_NOT_NULL(csv_reader_)) {
    delete csv_reader_;
    csv_reader_ = NULL;
  }
  if (OB_NOT_NULL(json_reader_)) {
    delete json_reader_;
    json_reader_ = NULL;
  }
  if (OB_NOT_NULL(parquet_reader_)) {
    delete parquet_reader_;
    parquet_reader_ = NULL;
  }
  if (OB_NOT_NULL(buf_)) {
    alloc_->free(buf_);
    buf_ = NULL;
  }
}

int ObFileScanner::init(common::ObIAllocator &alloc,
                        const common::ObString &full_path,
                        FileScanFormat format,
                        bool has_header,
                        const common::ObIArray<FileColumnMeta> &schema,
                        const common::ObIArray<int64_t> *projection)
{
  int ret = OB_SUCCESS;
  alloc_ = &alloc;
  format_ = format;
  has_header_ = has_header;
  path_ = full_path;
  for (int64_t i = 0; OB_SUCC(ret) && i < schema.count(); ++i) {
    if (OB_FAIL(schema_.push_back(schema.at(i)))) {
    } else { /* do nothing */ }
  }
  if (OB_SUCC(ret) && NULL != projection) {
    for (int64_t i = 0; OB_SUCC(ret) && i < projection->count(); ++i) {
      if (OB_FAIL(projection_.push_back(projection->at(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (FileScanFormat::CSV == format) {
      ret = init_csv();
    } else if (FileScanFormat::JSON == format) {
      ret = init_json();
    } else if (FileScanFormat::PARQUET == format) {
      ret = init_parquet();
    } else if (FileScanFormat::GLOB == format) {
      ret = init_glob();
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

int ObFileScanner::init_csv()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObFileScanUtils::read_whole_file(path_, *alloc_, buf_, buf_len_))) {
  } else {
    csv_reader_ = new ObCsvFileReader();
    if (OB_ISNULL(csv_reader_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(csv_reader_->init(*alloc_, buf_, buf_len_))) {
    } else if (OB_FAIL(check_csv_header())) {
    } else { /* do nothing */ }
  }
  return ret;
}

int ObFileScanner::check_csv_header()
{
  int ret = OB_SUCCESS;
  if (!has_header_ || schema_.count() == 0) {
    // nothing to check
  } else {
    common::ObSEArray<common::ObString, 16> fields;
    common::ObArray<common::ObString> names;
    if (OB_FAIL(csv_reader_->next_record(fields))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file has no header");
    } else if (fields.count() != schema_.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file has been changed");
    } else if (OB_FAIL(build_csv_column_names(*alloc_, fields, names))) {
    } else if (names.count() != schema_.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file has been changed");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
        if (0 != compare_string(names.at(i), schema_.at(i).name_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file has been changed");
        }
      }
    }
  }
  return ret;
}

int ObFileScanner::init_json()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObFileScanUtils::read_whole_file(path_, *alloc_, buf_, buf_len_))) {
  } else {
    json_reader_ = new ObJsonFileReader();
    if (OB_ISNULL(json_reader_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(json_reader_->init(*alloc_, buf_, buf_len_))) {
    } else { /* do nothing */ }
  }
  return ret;
}

int ObFileScanner::init_parquet()
{
  int ret = OB_SUCCESS;
  parquet_reader_ = new ObParquetReader();
  if (OB_ISNULL(parquet_reader_)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (projection_.count() > 0 && OB_FAIL(parquet_reader_->set_projection(projection_))) {
  } else if (OB_FAIL(parquet_reader_->init(*alloc_, path_))) {
  } else {
    // make sure the file is not changed after the schema was inferred
    common::ObSEArray<FileColumnMeta, 8> cur_schema;
    if (OB_FAIL(parquet_reader_->get_schema(cur_schema))) {
    } else if (cur_schema.count() != schema_.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file has been changed");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < cur_schema.count(); ++i) {
        if (0 != compare_string(cur_schema.at(i).name_, schema_.at(i).name_)
            || cur_schema.at(i).type_ != schema_.at(i).type_) {
          ret = OB_ERR_UNEXPECTED;
          LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file has been changed");
        }
      }
    }
  }
  return ret;
}

int ObFileScanner::init_glob()
{
  int ret = OB_SUCCESS;
  // the glob pattern is stored in the path of the schema
  const common::ObString &pattern = path_;
  int64_t sep = -1;
  for (int64_t i = 0; i < pattern.length(); ++i) {
    if ('/' == pattern.ptr()[i]) {
      sep = i;
    }
  }
  common::ObString dir;
  common::ObString file_pattern;
  if (sep < 0) {
    dir = common::ObString(1, ".");
    file_pattern = pattern;
  } else {
    dir = common::ObString(sep, pattern.ptr());
    file_pattern = common::ObString(pattern.length() - sep - 1, pattern.ptr() + sep + 1);
  }
  if (dir.empty()) {
    dir = common::ObString(1, "/");
  }
  char dir_buf[PATH_MAX + 1];
  char pattern_buf[PATH_MAX + 1];
  if (OB_FAIL(to_c_string(dir, dir_buf, sizeof(dir_buf)))) {
  } else if (OB_FAIL(to_c_string(file_pattern, pattern_buf, sizeof(pattern_buf)))) {
  }
#ifndef _WIN32
  DIR *dp = OB_SUCC(ret) ? opendir(dir_buf) : NULL;
  if (NULL == dp) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST);
  } else {
    struct dirent *entry = NULL;
    while (OB_SUCC(ret) && NULL != (entry = readdir(dp))) {
      const char *name = entry->d_name;
      if ('.' == name[0] && ('\0' == name[1] || ('.' == name[1] && '\0' == name[2]))) {
        continue;
      }
      if (match_pattern(pattern_buf, name)) {
        common::ObString full;
        char buf[PATH_MAX + 1];
        const int64_t len = snprintf(buf, sizeof(buf), "%s/%s", dir_buf, name);
        if (OB_FAIL(ob_write_string(*alloc_, common::ObString(len, buf), full))) {
        } else if (OB_FAIL(glob_files_.push_back(full))) {
        }
      }
    }
    closedir(dp);
  }
#else
  char find_pattern[PATH_MAX + 1];
  const int64_t len = snprintf(find_pattern, sizeof(find_pattern), "%s\\%s", dir_buf, pattern_buf);
  WIN32_FIND_DATAA find_data;
  HANDLE handle = OB_SUCC(ret) ? FindFirstFileA(find_pattern, &find_data) : INVALID_HANDLE_VALUE;
  if (OB_FAIL(ret)) {
  } else if (INVALID_HANDLE_VALUE == handle) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST);
  } else {
    do {
      const char *name = find_data.cFileName;
      if ('.' == name[0] && ('\0' == name[1] || ('.' == name[1] && '\0' == name[2]))) {
        continue;
      }
      if (match_pattern(pattern_buf, name)) {
        common::ObString full;
        char buf[PATH_MAX + 1];
        const int64_t full_len = snprintf(buf, sizeof(buf), "%s\\%s", dir_buf, name);
        if (OB_FAIL(ob_write_string(*alloc_, common::ObString(full_len, buf), full))) {
        } else if (OB_FAIL(glob_files_.push_back(full))) {
        }
      }
    } while (FindNextFileA(handle, &find_data));
    FindClose(handle);
  }
#endif
  // keep the result stable
  for (int64_t i = 1; OB_SUCC(ret) && i < glob_files_.count(); ++i) {
    common::ObString value = glob_files_.at(i);
    int64_t j = i - 1;
    while (j >= 0 && compare_string(glob_files_.at(j), value) > 0) {
      glob_files_.at(j + 1) = glob_files_.at(j);
      --j;
    }
    glob_files_.at(j + 1) = value;
  }
  return ret;
}

int ObFileScanner::next_row(common::ObIArray<FileCell> &row)
{
  int ret = OB_SUCCESS;
  row.reuse();
  if (FileScanFormat::CSV == format_) {
    ret = next_csv_row(row);
  } else if (FileScanFormat::JSON == format_) {
    ret = next_json_row(row);
  } else if (FileScanFormat::PARQUET == format_) {
    ret = parquet_reader_->next_row(row);
  } else if (FileScanFormat::GLOB == format_) {
    ret = next_glob_row(row);
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret)) {
    ++row_count_;
  }
  return ret;
}

int ObFileScanner::next_csv_row(common::ObIArray<FileCell> &row)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<common::ObString, 16> fields;
  if (OB_FAIL(csv_reader_->next_record(fields))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < schema_.count(); ++i) {
      FileCell cell;
      if (i < fields.count()) {
        if (OB_FAIL(ObFileScanUtils::convert_cell(fields.at(i), schema_.at(i).type_, cell))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(row.push_back(cell))) {
      }
    }
  }
  return ret;
}

int ObFileScanner::next_json_row(common::ObIArray<FileCell> &row)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<JsonKeyValue, 16> kvs;
  if (OB_FAIL(json_reader_->next_record(kvs))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < schema_.count(); ++i) {
      FileCell cell;
      cell.set_null();
      for (int64_t j = 0; OB_SUCC(ret) && j < kvs.count(); ++j) {
        if (0 == compare_string(schema_.at(i).name_, kvs.at(j).key_)) {
          const JsonKeyValue &kv = kvs.at(j);
          const FileColumnType type = schema_.at(i).type_;
          if (JSON_VALUE_NULL == kv.type_) {
            cell.set_null();
          } else if (JSON_VALUE_STRING == kv.type_) {
            if (OB_FAIL(ObFileScanUtils::convert_cell(kv.str_, type, cell))) {
            }
          } else if (JSON_VALUE_INT == kv.type_) {
            cell.is_null_ = false;
            if (FileColumnType::DOUBLE == type) {
              cell.dbl_val_ = static_cast<double>(kv.int_val_);
            } else if (FileColumnType::BOOLEAN == type) {
              cell.bool_val_ = (0 != kv.int_val_);
            } else if (FileColumnType::VARCHAR == type) {
              char buf[32];
              const int64_t len = snprintf(buf, sizeof(buf), "%ld", kv.int_val_);
              if (OB_FAIL(ob_write_string(*alloc_, common::ObString(len, buf), cell.str_))) {
              }
            } else {
              cell.int_val_ = kv.int_val_;
            }
          } else if (JSON_VALUE_DOUBLE == kv.type_) {
            cell.is_null_ = false;
            if (FileColumnType::DOUBLE == type) {
              cell.dbl_val_ = kv.dbl_val_;
            } else if (FileColumnType::BOOLEAN == type) {
              cell.bool_val_ = (0.0 != kv.dbl_val_);
            } else if (FileColumnType::VARCHAR == type) {
              char buf[64];
              const int64_t len = snprintf(buf, sizeof(buf), "%.15g", kv.dbl_val_);
              if (OB_FAIL(ob_write_string(*alloc_, common::ObString(len, buf), cell.str_))) {
              }
            } else {
              cell.int_val_ = static_cast<int64_t>(kv.dbl_val_);
            }
          } else {
            cell.is_null_ = false;
            if (FileColumnType::BOOLEAN == type) {
              cell.bool_val_ = kv.bool_val_;
            } else if (FileColumnType::VARCHAR == type) {
              cell.str_ = common::ObString(kv.bool_val_ ? 4 : 5, kv.bool_val_ ? "true" : "false");
            } else {
              cell.int_val_ = kv.bool_val_ ? 1 : 0;
            }
          }
          break;
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(row.push_back(cell))) {
      }
    }
  }
  return ret;
}

int ObFileScanner::next_glob_row(common::ObIArray<FileCell> &row)
{
  int ret = OB_SUCCESS;
  if (glob_idx_ >= glob_files_.count()) {
    ret = OB_ITER_END;
  } else {
    FileCell cell;
    cell.is_null_ = false;
    cell.str_ = glob_files_.at(glob_idx_++);
    if (OB_FAIL(row.push_back(cell))) {
    } else { /* do nothing */ }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
