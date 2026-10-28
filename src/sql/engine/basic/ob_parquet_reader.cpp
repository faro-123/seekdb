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

#include "sql/engine/basic/ob_parquet_reader.h"
#include "lib/allocator/ob_allocator.h"
#include "share/ob_errno.h"
#include <string.h>
#include <limits.h>

namespace oceanbase
{
namespace sql
{

namespace
{

// parquet physical types
enum ParquetType
{
  PARQUET_TYPE_BOOLEAN = 0,
  PARQUET_TYPE_INT32 = 1,
  PARQUET_TYPE_INT64 = 2,
  PARQUET_TYPE_INT96 = 3,
  PARQUET_TYPE_FLOAT = 4,
  PARQUET_TYPE_DOUBLE = 5,
  PARQUET_TYPE_BYTE_ARRAY = 6,
  PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY = 7,
};

// parquet field repetition types
enum ParquetRepetition
{
  PARQUET_REPETITION_REQUIRED = 0,
  PARQUET_REPETITION_OPTIONAL = 1,
  PARQUET_REPETITION_REPEATED = 2,
};

// parquet converted types
enum ParquetConvertedType
{
  PARQUET_CONVERTED_UTF8 = 0,
  PARQUET_CONVERTED_DATE = 6,
  PARQUET_CONVERTED_TIME_MILLIS = 7,
  PARQUET_CONVERTED_TIME_MICROS = 8,
  PARQUET_CONVERTED_TIMESTAMP_MILLIS = 9,
  PARQUET_CONVERTED_TIMESTAMP_MICROS = 10,
};

// simplified logical types
enum ParquetLogicalType
{
  PARQUET_LOGICAL_NONE = 0,
  PARQUET_LOGICAL_STRING,
  PARQUET_LOGICAL_DATE,
  PARQUET_LOGICAL_TIMESTAMP_MILLIS,
  PARQUET_LOGICAL_TIMESTAMP_MICROS,
  PARQUET_LOGICAL_TIMESTAMP_NANOS,
  PARQUET_LOGICAL_INTEGER,
};

// parquet encodings
enum ParquetEncoding
{
  PARQUET_ENCODING_PLAIN = 0,
  PARQUET_ENCODING_PLAIN_DICTIONARY = 2,
  PARQUET_ENCODING_RLE = 3,
  PARQUET_ENCODING_BIT_PACKED = 4,
  PARQUET_ENCODING_RLE_DICTIONARY = 8,
};

// parquet compression codecs
enum ParquetCodec
{
  PARQUET_CODEC_UNCOMPRESSED = 0,
  PARQUET_CODEC_SNAPPY = 1,
  PARQUET_CODEC_GZIP = 2,
};

// parquet page types
enum ParquetPageType
{
  PARQUET_PAGE_DATA = 0,
  PARQUET_PAGE_INDEX = 1,
  PARQUET_PAGE_DICTIONARY = 2,
  PARQUET_PAGE_DATA_V2 = 3,
};

// thrift compact protocol types
enum ThriftType
{
  THRIFT_STOP = 0,
  THRIFT_BOOLEAN_TRUE = 1,
  THRIFT_BOOLEAN_FALSE = 2,
  THRIFT_BYTE = 3,
  THRIFT_I16 = 4,
  THRIFT_I32 = 5,
  THRIFT_I64 = 6,
  THRIFT_DOUBLE = 7,
  THRIFT_BINARY = 8,
  THRIFT_LIST = 9,
  THRIFT_SET = 10,
  THRIFT_MAP = 11,
  THRIFT_STRUCT = 12,
};

class ThriftCompactReader
{
public:
  ThriftCompactReader(const char *data, int64_t len)
    : data_(data), len_(len), pos_(0), last_field_id_(0) {}
  ~ThriftCompactReader() {}

  int read_field_begin(int16_t &type, int16_t &field_id)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_ITER_END;
    } else {
      const uint8_t byte = static_cast<uint8_t>(data_[pos_++]);
      type = byte & 0x0F;
      if (THRIFT_STOP == type) {
        // the stop field only has the type byte
        ret = OB_ITER_END;
      } else {
        const int16_t delta = (byte & 0xF0) >> 4;
        if (0 == delta) {
          int64_t id = 0;
          if (OB_FAIL(read_varint(id))) {
          } else {
            field_id = static_cast<int16_t>(id);
            last_field_id_ = field_id;
          }
        } else {
          field_id = last_field_id_ + delta;
          last_field_id_ = field_id;
        }
      }
    }
    return ret;
  }

  int read_varint(int64_t &value)
  {
    int ret = OB_SUCCESS;
    uint64_t result = 0;
    int shift = 0;
    while (pos_ < len_) {
      const uint8_t byte = static_cast<uint8_t>(data_[pos_++]);
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
      if (shift > 63) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
    }
    if (OB_SUCC(ret)) {
      value = static_cast<int64_t>(result);
    }
    return ret;
  }

  int read_zigzag(int64_t &value)
  {
    int ret = OB_SUCCESS;
    int64_t raw = 0;
    if (OB_FAIL(read_varint(raw))) {
    } else {
      value = (raw >> 1) ^ -(raw & 1);
    }
    return ret;
  }

  int read_i32(int32_t &value)
  {
    int ret = OB_SUCCESS;
    int64_t tmp = 0;
    if (OB_FAIL(read_zigzag(tmp))) {
    } else {
      value = static_cast<int32_t>(tmp);
    }
    return ret;
  }

  int read_i64(int64_t &value)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(read_zigzag(value))) {
    } else { /* do nothing */ }
    return ret;
  }

  int read_double(double &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ + 8 > len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      uint64_t bits = 0;
      for (int64_t i = 0; i < 8; ++i) {
        bits |= static_cast<uint64_t>(static_cast<uint8_t>(data_[pos_ + i])) << (8 * i);
      }
      pos_ += 8;
      MEMCPY(&value, &bits, sizeof(value));
    }
    return ret;
  }

  int read_binary(const char *&ptr, int32_t &len)
  {
    int ret = OB_SUCCESS;
    int64_t size = 0;
    if (OB_FAIL(read_varint(size))) {
    } else if (size < 0 || pos_ + size > len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ptr = data_ + pos_;
      len = static_cast<int32_t>(size);
      pos_ += size;
    }
    return ret;
  }

  int read_list_begin(int32_t &size, int8_t &elem_type)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const uint8_t byte = static_cast<uint8_t>(data_[pos_++]);
      const int32_t header = byte >> 4;
      elem_type = byte & 0x0F;
      if (15 == header) {
        int64_t real_size = 0;
        if (OB_FAIL(read_varint(real_size))) {
        } else {
          size = static_cast<int32_t>(real_size);
        }
      } else {
        size = header;
      }
    }
    return ret;
  }

  int skip(int16_t type)
  {
    int ret = OB_SUCCESS;
    switch (type) {
    case THRIFT_BOOLEAN_TRUE:
    case THRIFT_BOOLEAN_FALSE:
      break;
    case THRIFT_BYTE:
      if (pos_ < len_) {
        ++pos_;
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    case THRIFT_I16:
    case THRIFT_I32:
    case THRIFT_I64: {
      int64_t tmp = 0;
      if (OB_FAIL(read_varint(tmp))) {
      }
      break;
    }
    case THRIFT_DOUBLE:
      if (pos_ + 8 <= len_) {
        pos_ += 8;
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    case THRIFT_BINARY: {
      const char *ptr = NULL;
      int32_t len = 0;
      if (OB_FAIL(read_binary(ptr, len))) {
      }
      break;
    }
    case THRIFT_LIST:
    case THRIFT_SET: {
      int32_t size = 0;
      int8_t elem_type = 0;
      if (OB_FAIL(read_list_begin(size, elem_type))) {
      } else {
        for (int32_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          if (OB_FAIL(skip(elem_type))) {
          }
        }
      }
      break;
    }
    case THRIFT_MAP: {
      int64_t size = 0;
      if (OB_FAIL(read_varint(size))) {
      } else if (size > 0) {
        if (pos_ >= len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const uint8_t types = static_cast<uint8_t>(data_[pos_++]);
          const int16_t key_type = (types & 0xF0) >> 4;
          const int16_t value_type = types & 0x0F;
          for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            if (OB_FAIL(skip(key_type)) || OB_FAIL(skip(value_type))) {
            }
          }
        }
      }
      break;
    }
    case THRIFT_STRUCT: {
      const int16_t saved_field_id = last_field_id_;
      last_field_id_ = 0;
      int16_t field_type = 0;
      int16_t field_id = 0;
      while (OB_SUCC(ret)) {
        if (OB_FAIL(read_field_begin(field_type, field_id))) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
          }
          break;
        }
        if (OB_FAIL(skip(field_type))) {
        }
      }
      last_field_id_ = saved_field_id;
      break;
    }
    default:
      ret = OB_ERR_UNEXPECTED;
      break;
    }
    return ret;
  }

  int64_t get_pos() const { return pos_; }
  int16_t get_last_field_id() const { return last_field_id_; }
  void set_last_field_id(int16_t field_id) { last_field_id_ = field_id; }

private:
  const char *data_;
  int64_t len_;
  int64_t pos_;
  int16_t last_field_id_;
};

// the field id delta is relative to the last field of the same struct
class ThriftStructGuard
{
public:
  explicit ThriftStructGuard(ThriftCompactReader &reader)
    : reader_(reader), saved_(reader.get_last_field_id())
  {
    reader_.set_last_field_id(0);
  }
  ~ThriftStructGuard() { reader_.set_last_field_id(saved_); }
private:
  ThriftCompactReader &reader_;
  int16_t saved_;
};

int snappy_uncompress(const char *input, int64_t input_len,
                       common::ObIAllocator &alloc, char *&output, int64_t &output_len)
{
  int ret = OB_SUCCESS;
  output = NULL;
  output_len = 0;
  int64_t pos = 0;
  uint64_t uncompressed_size = 0;
  int shift = 0;
  while (pos < input_len) {
    const uint8_t byte = static_cast<uint8_t>(input[pos++]);
    uncompressed_size |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
    if (shift > 63) {
      ret = OB_ERR_UNEXPECTED;
      break;
    }
  }
  if (OB_SUCC(ret)) {
    if (uncompressed_size > 1024L * 1024L * 1024L) {
      ret = OB_SIZE_OVERFLOW;
    } else if (OB_ISNULL(output = static_cast<char *>(alloc.alloc(uncompressed_size + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t out_pos = 0;
      while (OB_SUCC(ret) && pos < input_len) {
        const uint8_t tag = static_cast<uint8_t>(input[pos++]);
        if (0 == (tag & 0x03)) {
          // literal
          int64_t len = (tag >> 2) + 1;
          if (pos + len > input_len || out_pos + len > uncompressed_size) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          MEMCPY(output + out_pos, input + pos, len);
          pos += len;
          out_pos += len;
        } else {
          int64_t len = 0;
          int64_t offset = 0;
          if (1 == (tag & 0x03)) {
            len = 4 + ((tag >> 2) & 0x07);
            if (pos >= input_len) {
              ret = OB_ERR_UNEXPECTED;
              break;
            }
            offset = static_cast<int64_t>((tag & 0xE0) << 3) | static_cast<uint8_t>(input[pos]);
            pos += 1;
          } else if (2 == (tag & 0x03)) {
            len = 1 + (tag >> 2);
            if (pos + 2 > input_len) {
              ret = OB_ERR_UNEXPECTED;
              break;
            }
            offset = static_cast<uint8_t>(input[pos]) | (static_cast<int64_t>(static_cast<uint8_t>(input[pos + 1])) << 8);
            pos += 2;
          } else {
            len = 1 + (tag >> 2);
            if (pos + 4 > input_len) {
              ret = OB_ERR_UNEXPECTED;
              break;
            }
            offset = static_cast<int64_t>(static_cast<uint8_t>(input[pos]))
                     | (static_cast<int64_t>(static_cast<uint8_t>(input[pos + 1])) << 8)
                     | (static_cast<int64_t>(static_cast<uint8_t>(input[pos + 2])) << 16)
                     | (static_cast<int64_t>(static_cast<uint8_t>(input[pos + 3])) << 24);
            pos += 4;
          }
          if (offset <= 0 || offset > out_pos || out_pos + len > uncompressed_size) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          for (int64_t i = 0; i < len; ++i) {
            output[out_pos] = output[out_pos - offset];
            ++out_pos;
          }
        }
      }
      if (OB_SUCC(ret) && out_pos != uncompressed_size) {
        ret = OB_ERR_UNEXPECTED;
      }
      if (OB_SUCC(ret)) {
        output_len = out_pos;
      }
    }
  }
  return ret;
}

// decode the rle/bit-packing hybrid values
int decode_rle_hybrid(const char *data, int64_t len, int32_t bit_width,
                      int64_t num_values, common::ObArray<int64_t> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  while (OB_SUCC(ret) && values.count() < num_values && pos < len) {
    int64_t header = 0;
    int64_t shift = 0;
    bool got = false;
    while (pos < len) {
      const uint8_t byte = static_cast<uint8_t>(data[pos++]);
      header |= static_cast<int64_t>(byte & 0x7F) << shift;
      got = true;
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
      if (shift > 63) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (!got) {
      ret = OB_ERR_UNEXPECTED;
    } else if (header & 1) {
      // bit packed run
      const int64_t count = (header >> 1) * 8;
      const int64_t bytes = (bit_width * count) / 8;
      if (pos + bytes > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        int64_t value = 0;
        for (int32_t b = 0; b < bit_width; ++b) {
          const int64_t bit_index = i * bit_width + b;
          const uint8_t byte = static_cast<uint8_t>(data[pos + bit_index / 8]);
          if (byte & (1 << (bit_index % 8))) {
            value |= (1LL << b);
          }
        }
        if (values.count() >= num_values) {
          break;
        }
        if (OB_FAIL(values.push_back(value))) {
          break;
        }
      }
      pos += bytes;
    } else {
      // rle run
      const int64_t count = header >> 1;
      const int64_t byte_width = (bit_width + 7) / 8;
      if (pos + byte_width > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      int64_t value = 0;
      for (int64_t i = 0; i < byte_width; ++i) {
        value |= static_cast<int64_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i);
      }
      pos += byte_width;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        if (values.count() >= num_values) {
          break;
        }
        if (OB_FAIL(values.push_back(value))) {
          break;
        }
      }
    }
  }
  return ret;
}

} // namespace

ObParquetReader::ObParquetReader()
  : alloc_(NULL), buf_(NULL), buf_len_(0), footer_(NULL), footer_len_(0),
    schema_(), chunks_(), row_groups_(), all_cells_(), projection_(),
    row_count_(0), cur_row_(0)
{
}

ObParquetReader::~ObParquetReader()
{
}

int ObParquetReader::init(common::ObIAllocator &alloc, const common::ObString &path)
{
  int ret = OB_SUCCESS;
  alloc_ = &alloc;
  if (OB_FAIL(read_file(path))) {
  } else if (OB_FAIL(parse_footer())) {
  } else if (OB_FAIL(parse_schema())) {
  } else if (OB_FAIL(parse_row_groups())) {
  } else if (OB_FAIL(decode_row_groups())) {
  } else { /* do nothing */ }
  return ret;
}

int ObParquetReader::read_file(const common::ObString &path)
{
  int ret = OB_SUCCESS;
  char path_buf[4096];
  FILE *fp = NULL;
  if (OB_UNLIKELY(path.length() >= static_cast<int64_t>(sizeof(path_buf)))) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    fp = fopen(path_buf, "rb");
  }
  if (OB_FAIL(ret)) {
  } else if (NULL == fp) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST);
  } else {
    if (0 != fseek(fp, 0, SEEK_END)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const long file_size = ftell(fp);
      if (file_size < 8) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file is invalid");
      } else if (file_size > ObFileScanUtils::MAX_FILE_SIZE) {
        ret = OB_SIZE_OVERFLOW;
        LOG_USER_ERROR(OB_SIZE_OVERFLOW);
      } else {
        buf_len_ = static_cast<int64_t>(file_size);
        if (0 != fseek(fp, 0, SEEK_SET)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(buf_ = static_cast<char *>(alloc_->alloc(buf_len_ + 1)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (buf_len_ != static_cast<int64_t>(fread(buf_, 1, buf_len_, fp))) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          buf_[buf_len_] = '\0';
        }
      }
    }
    fclose(fp);
  }
  return ret;
}

int ObParquetReader::parse_footer()
{
  int ret = OB_SUCCESS;
  // the last 8 bytes are the footer length and the magic number
  const int32_t footer_size = static_cast<int32_t>(static_cast<uint8_t>(buf_[buf_len_ - 8]))
                              | (static_cast<int32_t>(static_cast<uint8_t>(buf_[buf_len_ - 7])) << 8)
                              | (static_cast<int32_t>(static_cast<uint8_t>(buf_[buf_len_ - 6])) << 16)
                              | (static_cast<int32_t>(static_cast<uint8_t>(buf_[buf_len_ - 5])) << 24);
  if (footer_size <= 0 || buf_len_ - 8 < footer_size) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file is invalid");
  } else {
    footer_ = buf_ + buf_len_ - 8 - footer_size;
    footer_len_ = footer_size;
  }
  return ret;
}

int ObParquetReader::parse_schema()
{
  int ret = OB_SUCCESS;
  ThriftCompactReader reader(footer_, footer_len_);
  int16_t type = 0;
  int16_t field_id = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(reader.read_field_begin(type, field_id))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
      break;
    }
    if (2 == field_id) {
      // the schema list
      int32_t size = 0;
      int8_t elem_type = 0;
      if (OB_FAIL(reader.read_list_begin(size, elem_type))) {
      } else {
        for (int32_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ThriftStructGuard element_guard(reader);
          ColumnSchema element;
          int16_t sub_type = 0;
          int16_t sub_id = 0;
          while (OB_SUCC(ret)) {
            if (OB_FAIL(reader.read_field_begin(sub_type, sub_id))) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
              }
              break;
            }
            if (1 == sub_id) {
              if (OB_FAIL(reader.read_i32(element.type_))) {
              }
            } else if (3 == sub_id) {
              if (OB_FAIL(reader.read_i32(element.repetition_))) {
              }
            } else if (4 == sub_id) {
              const char *name = NULL;
              int32_t name_len = 0;
              if (OB_FAIL(reader.read_binary(name, name_len))) {
              } else {
                element.name_.assign_ptr(name, name_len);
              }
            } else if (5 == sub_id) {
              int32_t num_children = 0;
              if (OB_FAIL(reader.read_i32(num_children))) {
              } else {
                element.num_children_ = num_children;
              }
            } else if (6 == sub_id) {
              if (OB_FAIL(reader.read_i32(element.converted_type_))) {
              }
            } else if (2 == sub_id) {
              if (OB_FAIL(reader.read_i32(element.type_length_))) {
              }
            } else if (10 == sub_id) {
              // the logical type
              ThriftStructGuard logic_guard(reader);
              int16_t logic_type = 0;
              int16_t logic_id = 0;
              while (OB_SUCC(ret)) {
                if (OB_FAIL(reader.read_field_begin(logic_type, logic_id))) {
                  if (OB_ITER_END == ret) {
                    ret = OB_SUCCESS;
                  }
                  break;
                }
                if (1 == logic_id) {
                  element.logical_type_ = PARQUET_LOGICAL_STRING;
                  if (OB_FAIL(reader.skip(logic_type))) {
                  }
                } else if (6 == logic_id) {
                  element.logical_type_ = PARQUET_LOGICAL_DATE;
                  if (OB_FAIL(reader.skip(logic_type))) {
                  }
                } else if (8 == logic_id) {
                  // the timestamp logical type
                  ThriftStructGuard ts_guard(reader);
                  int16_t ts_type = 0;
                  int16_t ts_id = 0;
                  while (OB_SUCC(ret)) {
                    if (OB_FAIL(reader.read_field_begin(ts_type, ts_id))) {
                      if (OB_ITER_END == ret) {
                        ret = OB_SUCCESS;
                      }
                      break;
                    }
                    if (2 == ts_id) {
                      ThriftStructGuard unit_guard(reader);
                      int16_t unit_type = 0;
                      int16_t unit_id = 0;
                      while (OB_SUCC(ret)) {
                        if (OB_FAIL(reader.read_field_begin(unit_type, unit_id))) {
                          if (OB_ITER_END == ret) {
                            ret = OB_SUCCESS;
                          }
                          break;
                        }
                        if (1 == unit_id) {
                          element.logical_type_ = PARQUET_LOGICAL_TIMESTAMP_MILLIS;
                          if (OB_FAIL(reader.skip(unit_type))) {
                          }
                        } else if (2 == unit_id) {
                          element.logical_type_ = PARQUET_LOGICAL_TIMESTAMP_MICROS;
                          if (OB_FAIL(reader.skip(unit_type))) {
                          }
                        } else if (3 == unit_id) {
                          element.logical_type_ = PARQUET_LOGICAL_TIMESTAMP_NANOS;
                          if (OB_FAIL(reader.skip(unit_type))) {
                          }
                        } else {
                          if (OB_FAIL(reader.skip(unit_type))) {
                          }
                        }
                      }
                    } else {
                      if (OB_FAIL(reader.skip(ts_type))) {
                      }
                    }
                  }
                } else {
                  if (OB_FAIL(reader.skip(logic_type))) {
                  }
                }
              }
            } else {
              if (OB_FAIL(reader.skip(sub_type))) {
              }
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(schema_.push_back(element))) {
          }
        }
      }
    } else {
      if (OB_FAIL(reader.skip(type))) {
      }
    }
  }
  // the first element is the root of the schema
  if (OB_SUCC(ret)) {
    if (schema_.count() < 2) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file has no column");
    } else {
      common::ObArray<ColumnSchema> leaves;
      for (int64_t i = 1; i < schema_.count(); ++i) {
        if (schema_.at(i).num_children_ > 0) {
          // the nested schema is not supported
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the nested parquet schema");
          break;
        }
        if (OB_FAIL(leaves.push_back(schema_.at(i)))) {
          break;
        }
      }
      if (OB_SUCC(ret)) {
        schema_.reuse();
        for (int64_t i = 0; i < leaves.count(); ++i) {
          if (OB_FAIL(schema_.push_back(leaves.at(i)))) {
            break;
          }
        }
      }
    }
  }
  return ret;
}

int ObParquetReader::parse_row_groups()
{
  int ret = OB_SUCCESS;
  ThriftCompactReader reader(footer_, footer_len_);
  int16_t type = 0;
  int16_t field_id = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(reader.read_field_begin(type, field_id))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
      break;
    }
    if (4 == field_id) {
      int32_t size = 0;
      int8_t elem_type = 0;
      if (OB_FAIL(reader.read_list_begin(size, elem_type))) {
          } else {
        for (int32_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ThriftStructGuard row_group_guard(reader);
          int64_t num_rows = 0;
          common::ObArray<ChunkInfo> chunks;
          int16_t sub_type = 0;
          int16_t sub_id = 0;
          while (OB_SUCC(ret)) {
            if (OB_FAIL(reader.read_field_begin(sub_type, sub_id))) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
              }
              break;
            }
            if (1 == sub_id) {
              // the column chunks
              int32_t chunk_size = 0;
              int8_t chunk_elem_type = 0;
              if (OB_FAIL(reader.read_list_begin(chunk_size, chunk_elem_type))) {
                break;
              }
              for (int32_t j = 0; OB_SUCC(ret) && j < chunk_size; ++j) {
                ThriftStructGuard chunk_guard(reader);
                ChunkInfo chunk;
                int16_t chunk_type = 0;
                int16_t chunk_id = 0;
                while (OB_SUCC(ret)) {
                  if (OB_FAIL(reader.read_field_begin(chunk_type, chunk_id))) {
                    if (OB_ITER_END == ret) {
                      ret = OB_SUCCESS;
                    }
                    break;
                  }
                  if (3 == chunk_id) {
                    // the column meta data
                    ThriftStructGuard meta_guard(reader);
                    int16_t meta_type = 0;
                    int16_t meta_id = 0;
                    while (OB_SUCC(ret)) {
                      if (OB_FAIL(reader.read_field_begin(meta_type, meta_id))) {
                        if (OB_ITER_END == ret) {
                          ret = OB_SUCCESS;
                        }
                        break;
                      }
                      if (1 == meta_id) {
                        if (OB_FAIL(reader.read_i32(chunk.type_))) {
                        }
                      } else if (4 == meta_id) {
                        if (OB_FAIL(reader.read_i32(chunk.codec_))) {
                        }
                      } else if (5 == meta_id) {
                        if (OB_FAIL(reader.read_i64(chunk.num_values_))) {
                        }
                      } else if (9 == meta_id) {
                        if (OB_FAIL(reader.read_i64(chunk.data_page_offset_))) {
                        }
                      } else if (11 == meta_id) {
                        if (OB_FAIL(reader.read_i64(chunk.dictionary_page_offset_))) {
                        }
                      } else {
                        if (OB_FAIL(reader.skip(meta_type))) {
                        }
                      }
                    }
                  } else {
                    if (OB_FAIL(reader.skip(chunk_type))) {
                    }
                  }
                }
                if (OB_SUCC(ret) && OB_FAIL(chunks.push_back(chunk))) {
                  break;
                }
              }
            } else if (3 == sub_id) {
              if (OB_FAIL(reader.read_i64(num_rows))) {
              }
            } else {
              if (OB_FAIL(reader.skip(sub_type))) {
              }
            }
          }
          if (OB_SUCC(ret)) {
            RowGroupInfo row_group;
            row_group.num_rows_ = num_rows;
            row_group.chunk_begin_ = chunks_.count();
            row_group.chunk_count_ = chunks.count();
            for (int64_t j = 0; OB_SUCC(ret) && j < chunks.count(); ++j) {
              if (OB_FAIL(chunks_.push_back(chunks.at(j)))) {
                break;
              }
            }
            if (OB_SUCC(ret) && OB_FAIL(row_groups_.push_back(row_group))) {
            }
          }
        }
      }
    } else {
      if (OB_FAIL(reader.skip(type))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < row_groups_.count(); ++i) {
      row_count_ += row_groups_.at(i).num_rows_;
    }
  }
  return ret;
}

int ObParquetReader::get_schema(common::ObIArray<FileColumnMeta> &columns)
{
  int ret = OB_SUCCESS;
  columns.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < schema_.count(); ++i) {
    const ColumnSchema &element = schema_.at(i);
    FileColumnMeta meta;
    meta.name_ = element.name_;
    meta.nullable_ = (PARQUET_REPETITION_OPTIONAL == element.repetition_);
    if (PARQUET_TYPE_BOOLEAN == element.type_) {
      meta.type_ = FileColumnType::BOOLEAN;
    } else if (PARQUET_TYPE_INT32 == element.type_) {
      if (PARQUET_CONVERTED_DATE == element.converted_type_ || PARQUET_LOGICAL_DATE == element.logical_type_) {
        meta.type_ = FileColumnType::DATE;
      } else {
        meta.type_ = FileColumnType::BIGINT;
      }
    } else if (PARQUET_TYPE_INT64 == element.type_) {
      if (PARQUET_CONVERTED_TIMESTAMP_MILLIS == element.converted_type_
          || PARQUET_CONVERTED_TIMESTAMP_MICROS == element.converted_type_
          || PARQUET_LOGICAL_TIMESTAMP_MILLIS == element.logical_type_
          || PARQUET_LOGICAL_TIMESTAMP_MICROS == element.logical_type_
          || PARQUET_LOGICAL_TIMESTAMP_NANOS == element.logical_type_) {
        meta.type_ = FileColumnType::DATETIME;
      } else {
        meta.type_ = FileColumnType::BIGINT;
      }
    } else if (PARQUET_TYPE_INT96 == element.type_) {
      meta.type_ = FileColumnType::DATETIME;
    } else if (PARQUET_TYPE_FLOAT == element.type_ || PARQUET_TYPE_DOUBLE == element.type_) {
      meta.type_ = FileColumnType::DOUBLE;
    } else {
      meta.type_ = FileColumnType::VARCHAR;
    }
    if (OB_FAIL(columns.push_back(meta))) {
    } else { /* do nothing */ }
  }
  return ret;
}

int ObParquetReader::set_projection(const common::ObIArray<int64_t> &col_idxs)
{
  int ret = OB_SUCCESS;
  projection_.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < col_idxs.count(); ++i) {
    if (OB_FAIL(projection_.push_back(col_idxs.at(i)))) {
    } else { /* do nothing */ }
  }
  return ret;
}

bool ObParquetReader::is_projected(int64_t col_idx) const
{
  if (projection_.count() == 0) {
    return true;
  }
  for (int64_t i = 0; i < projection_.count(); ++i) {
    if (projection_.at(i) == col_idx) {
      return true;
    }
  }
  return false;
}

int ObParquetReader::copy_string(const char *data, int64_t len, common::ObString &out)
{
  int ret = OB_SUCCESS;
  if (len <= 0) {
    out = common::ObString(0, "");
  } else {
    char *buf = static_cast<char *>(alloc_->alloc(len));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, data, len);
      out = common::ObString(len, buf);
    }
  }
  return ret;
}

int ObParquetReader::read_page_header(int64_t offset, PageHeaderInfo &info)
{
  int ret = OB_SUCCESS;
  if (offset < 0 || offset >= buf_len_) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ThriftCompactReader reader(buf_ + offset, buf_len_ - offset);
    int16_t type = 0;
    int16_t field_id = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(reader.read_field_begin(type, field_id))) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
        break;
      }
      if (1 == field_id) {
        if (OB_FAIL(reader.read_i32(info.page_type))) {
        }
      } else if (2 == field_id) {
        if (OB_FAIL(reader.read_i32(info.uncompressed_size))) {
        }
      } else if (3 == field_id) {
        if (OB_FAIL(reader.read_i32(info.compressed_size))) {
        }
      } else if (5 == field_id) {
        // the data page header
        ThriftStructGuard data_page_guard(reader);
        int16_t sub_type = 0;
        int16_t sub_id = 0;
        while (OB_SUCC(ret)) {
          if (OB_FAIL(reader.read_field_begin(sub_type, sub_id))) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
            }
            break;
          }
          if (1 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.num_values))) {
            }
          } else if (2 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.encoding))) {
            }
          } else if (OB_FAIL(reader.skip(sub_type))) {
          }
        }
      } else if (7 == field_id) {
        // the dictionary page header
        ThriftStructGuard dict_page_guard(reader);
        int16_t sub_type = 0;
        int16_t sub_id = 0;
        while (OB_SUCC(ret)) {
          if (OB_FAIL(reader.read_field_begin(sub_type, sub_id))) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
            }
            break;
          }
          if (1 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.num_values))) {
            }
          } else if (2 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.encoding))) {
            }
          } else if (OB_FAIL(reader.skip(sub_type))) {
          }
        }
      } else if (8 == field_id) {
        // the data page header v2
        ThriftStructGuard data_page_v2_guard(reader);
        int16_t sub_type = 0;
        int16_t sub_id = 0;
        while (OB_SUCC(ret)) {
          if (OB_FAIL(reader.read_field_begin(sub_type, sub_id))) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
            }
            break;
          }
          if (1 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.num_values))) {
            }
          } else if (2 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.num_nulls))) {
            }
          } else if (4 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.encoding))) {
            }
          } else if (5 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.def_levels_byte_length))) {
            }
          } else if (6 == sub_id) {
            if (OB_FAIL(reader.read_i32(info.rep_levels_byte_length))) {
            }
          } else if (7 == sub_id) {
            // the boolean value is stored in the field type
            info.is_compressed = (THRIFT_BOOLEAN_TRUE == sub_type);
          } else if (OB_FAIL(reader.skip(sub_type))) {
          }
        }
      } else if (OB_FAIL(reader.skip(type))) {
      }
    }
    info.header_size = reader.get_pos();
  }
  return ret;
}

int ObParquetReader::decode_plain_values(const char *data, int64_t len, int64_t num_values,
                                         int32_t type, const ColumnSchema &element,
                                         common::ObArray<FileCell> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  int64_t bit_pos = 0;
  const int32_t logical_type = element.logical_type_;
  for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
    FileCell cell;
    switch (type) {
    case PARQUET_TYPE_BOOLEAN: {
      const int64_t byte_index = bit_pos / 8;
      if (byte_index >= len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const uint8_t byte = static_cast<uint8_t>(data[byte_index]);
      cell.is_null_ = false;
      cell.bool_val_ = (0 != (byte & (1 << (bit_pos % 8))));
      ++bit_pos;
      break;
    }
    case PARQUET_TYPE_INT32: {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      int32_t value = 0;
      for (int64_t j = 0; j < 4; ++j) {
        value |= static_cast<int32_t>(static_cast<uint8_t>(data[pos + j])) << (8 * j);
      }
      pos += 4;
      cell.is_null_ = false;
      cell.int_val_ = value;
      break;
    }
    case PARQUET_TYPE_INT64: {
      if (pos + 8 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      int64_t value = 0;
      for (int64_t j = 0; j < 8; ++j) {
        value |= static_cast<int64_t>(static_cast<uint8_t>(data[pos + j])) << (8 * j);
      }
      pos += 8;
      cell.is_null_ = false;
      if (PARQUET_LOGICAL_TIMESTAMP_MILLIS == logical_type) {
        cell.int_val_ = value * 1000;
      } else if (PARQUET_LOGICAL_TIMESTAMP_NANOS == logical_type) {
        cell.int_val_ = value / 1000;
      } else {
        cell.int_val_ = value;
      }
      break;
    }
    case PARQUET_TYPE_INT96: {
      if (pos + 12 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      uint64_t nanos = 0;
      for (int64_t j = 0; j < 8; ++j) {
        nanos |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + j])) << (8 * j);
      }
      uint32_t julian_day = 0;
      for (int64_t j = 0; j < 4; ++j) {
        julian_day |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 8 + j])) << (8 * j);
      }
      pos += 12;
      cell.is_null_ = false;
      cell.int_val_ = (static_cast<int64_t>(julian_day) - 2440588) * 24 * 3600 * 1000000
                      + static_cast<int64_t>(nanos / 1000);
      break;
    }
    case PARQUET_TYPE_FLOAT: {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      uint32_t bits = 0;
      for (int64_t j = 0; j < 4; ++j) {
        bits |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + j])) << (8 * j);
      }
      pos += 4;
      float value = 0.0f;
      MEMCPY(&value, &bits, sizeof(value));
      cell.is_null_ = false;
      cell.dbl_val_ = value;
      break;
    }
    case PARQUET_TYPE_DOUBLE: {
      if (pos + 8 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      uint64_t bits = 0;
      for (int64_t j = 0; j < 8; ++j) {
        bits |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + j])) << (8 * j);
      }
      pos += 8;
      double value = 0.0;
      MEMCPY(&value, &bits, sizeof(value));
      cell.is_null_ = false;
      cell.dbl_val_ = value;
      break;
    }
    case PARQUET_TYPE_BYTE_ARRAY: {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      int32_t size = 0;
      for (int64_t j = 0; j < 4; ++j) {
        size |= static_cast<int32_t>(static_cast<uint8_t>(data[pos + j])) << (8 * j);
      }
      pos += 4;
      if (size < 0 || pos + size > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      common::ObString text;
      if (OB_FAIL(copy_string(data + pos, size, text))) {
        break;
      }
      pos += size;
      cell.is_null_ = false;
      cell.str_ = text;
      break;
    }
    case PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY: {
      const int32_t type_length = element.type_length_;
      if (type_length <= 0 || pos + type_length > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      common::ObString text;
      if (OB_FAIL(copy_string(data + pos, type_length, text))) {
        break;
      }
      pos += type_length;
      cell.is_null_ = false;
      cell.str_ = text;
      break;
    }
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "the parquet column type");
      break;
    }
    if (OB_SUCC(ret) && OB_FAIL(values.push_back(cell))) {
    }
  }
  return ret;
}

int ObParquetReader::decode_dict_indices(const char *data, int64_t len, int64_t num_values,
                                         const common::ObArray<FileCell> &dict,
                                         common::ObArray<FileCell> &values)
{
  int ret = OB_SUCCESS;
  if (len < 1) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int32_t bit_width = static_cast<uint8_t>(data[0]);
    common::ObArray<int64_t> indices;
    if (OB_FAIL(decode_rle_hybrid(data + 1, len - 1, bit_width, num_values, indices))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < indices.count(); ++i) {
        const int64_t index = indices.at(i);
        if (index < 0 || index >= dict.count()) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        if (OB_FAIL(values.push_back(dict.at(index)))) {
        }
      }
    }
  }
  return ret;
}

int ObParquetReader::decode_column_chunk(int64_t col_idx, int64_t row_group_idx)
{
  int ret = OB_SUCCESS;
  const RowGroupInfo &row_group = row_groups_.at(row_group_idx);
  if (row_group.chunk_begin_ + col_idx >= chunks_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ChunkInfo &chunk = chunks_.at(row_group.chunk_begin_ + col_idx);
    const ColumnSchema &element = schema_.at(col_idx);
    const bool is_optional = (PARQUET_REPETITION_OPTIONAL == element.repetition_);
    int64_t row_begin = 0;
    for (int64_t i = 0; i < row_group_idx; ++i) {
      row_begin += row_groups_.at(i).num_rows_;
    }
    common::ObArray<FileCell> dict;
    common::ObArray<FileCell> chunk_values;
    int64_t offset = chunk.dictionary_page_offset_ > 0 ? chunk.dictionary_page_offset_ : chunk.data_page_offset_;
    int64_t values_read = 0;
    while (OB_SUCC(ret) && values_read < chunk.num_values_) {
      PageHeaderInfo info;
      if (OB_FAIL(read_page_header(offset, info))) {
        break;
      }
      const int64_t data_offset = offset + info.header_size;
      if (data_offset + info.compressed_size > buf_len_) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      if (PARQUET_PAGE_DICTIONARY == info.page_type) {
        // decode the dictionary of the column
        char *page_data = NULL;
        int64_t page_len = 0;
        if (PARQUET_CODEC_UNCOMPRESSED == chunk.codec_) {
          page_data = buf_ + data_offset;
          page_len = info.compressed_size;
        } else if (PARQUET_CODEC_SNAPPY == chunk.codec_) {
          if (OB_FAIL(snappy_uncompress(buf_ + data_offset, info.compressed_size, *alloc_, page_data, page_len))) {
            break;
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the parquet compression codec");
          break;
        }
        dict.reuse();
        if (OB_FAIL(decode_plain_values(page_data, page_len, info.num_values, chunk.type_,
                                        element, dict))) {
          break;
        }
        offset = data_offset + info.compressed_size;
        continue;
      }
      if (PARQUET_PAGE_DATA != info.page_type && PARQUET_PAGE_DATA_V2 != info.page_type) {
        // the index page is not needed
        offset = data_offset + info.compressed_size;
        continue;
      }
      common::ObArray<int64_t> def_levels;
      int64_t present_count = info.num_values;
      char *page_data = NULL;
      int64_t page_len = 0;
      int64_t pos = 0;
      if (PARQUET_PAGE_DATA == info.page_type) {
        // the data page v1, the whole page is compressed
        if (PARQUET_CODEC_UNCOMPRESSED == chunk.codec_) {
          page_data = buf_ + data_offset;
          page_len = info.compressed_size;
        } else if (PARQUET_CODEC_SNAPPY == chunk.codec_) {
          if (OB_FAIL(snappy_uncompress(buf_ + data_offset, info.compressed_size, *alloc_, page_data, page_len))) {
            break;
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the parquet compression codec");
          break;
        }
        if (is_optional) {
          if (pos + 4 > page_len) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          int32_t len = 0;
          for (int64_t j = 0; j < 4; ++j) {
            len |= static_cast<int32_t>(static_cast<uint8_t>(page_data[pos + j])) << (8 * j);
          }
          pos += 4;
          if (pos + len > page_len || OB_FAIL(decode_rle_hybrid(page_data + pos, len, 1, info.num_values, def_levels))) {
            if (OB_SUCC(ret)) {
              ret = OB_ERR_UNEXPECTED;
            }
            break;
          }
          pos += len;
          present_count = 0;
          for (int64_t j = 0; j < def_levels.count(); ++j) {
            if (def_levels.at(j) > 0) {
              ++present_count;
            }
          }
        }
        page_len -= pos;
        page_data += pos;
      } else {
        // the data page v2, the levels are not compressed
        page_data = buf_ + data_offset;
        page_len = info.compressed_size;
        pos += info.rep_levels_byte_length;
        if (is_optional) {
          if (pos + info.def_levels_byte_length > page_len) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          if (OB_FAIL(decode_rle_hybrid(page_data + pos, info.def_levels_byte_length, 1, info.num_values, def_levels))) {
            break;
          }
          present_count = 0;
          for (int64_t j = 0; j < def_levels.count(); ++j) {
            if (def_levels.at(j) > 0) {
              ++present_count;
            }
          }
          pos += info.def_levels_byte_length;
        }
        page_len -= pos;
        page_data += pos;
        if (info.is_compressed) {
          char *uncompressed = NULL;
          int64_t uncompressed_len = 0;
          if (PARQUET_CODEC_UNCOMPRESSED == chunk.codec_) {
            uncompressed = page_data;
            uncompressed_len = page_len;
          } else if (PARQUET_CODEC_SNAPPY == chunk.codec_) {
            if (OB_FAIL(snappy_uncompress(page_data, page_len, *alloc_, uncompressed, uncompressed_len))) {
              break;
            }
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "the parquet compression codec");
            break;
          }
          page_data = uncompressed;
          page_len = uncompressed_len;
        }
      }
      common::ObArray<FileCell> present_values;
      if (PARQUET_ENCODING_PLAIN == info.encoding) {
        if (OB_FAIL(decode_plain_values(page_data, page_len, present_count, chunk.type_,
                                        element, present_values))) {
          break;
        }
      } else if (PARQUET_ENCODING_PLAIN_DICTIONARY == info.encoding
                 || PARQUET_ENCODING_RLE_DICTIONARY == info.encoding) {
        if (OB_FAIL(decode_dict_indices(page_data, page_len, present_count, dict, present_values))) {
          break;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "the parquet encoding");
        break;
      }
      // merge the definition levels with the values
      int64_t value_index = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < info.num_values; ++i) {
        FileCell cell;
        if (is_optional && (i >= def_levels.count() || 0 == def_levels.at(i))) {
          cell.set_null();
        } else {
          if (value_index >= present_values.count()) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          cell = present_values.at(value_index++);
        }
        if (OB_FAIL(chunk_values.push_back(cell))) {
          break;
        }
      }
      values_read += info.num_values;
      offset = data_offset + info.compressed_size;
    }
    if (OB_SUCC(ret)) {
      // store the values of the column
      for (int64_t i = 0; OB_SUCC(ret) && i < chunk_values.count(); ++i) {
        const int64_t index = col_idx * row_count_ + row_begin + i;
        if (index >= all_cells_.count()) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        all_cells_[index] = chunk_values.at(i);
      }
    }
  }
  return ret;
}

int ObParquetReader::decode_row_groups()
{
  int ret = OB_SUCCESS;
  const int64_t col_count = schema_.count();
  for (int64_t i = 0; OB_SUCC(ret) && i < row_count_ * col_count; ++i) {
    if (OB_FAIL(all_cells_.push_back(FileCell()))) {
      break;
    }
  }
  for (int64_t rg = 0; OB_SUCC(ret) && rg < row_groups_.count(); ++rg) {
    for (int64_t c = 0; OB_SUCC(ret) && c < col_count; ++c) {
      if (is_projected(c) && OB_FAIL(decode_column_chunk(c, rg))) {
        break;
      }
    }
  }
  return ret;
}

int ObParquetReader::next_row(common::ObIArray<FileCell> &row)
{
  int ret = OB_SUCCESS;
  row.reuse();
  if (cur_row_ >= row_count_) {
    ret = OB_ITER_END;
  } else {
    const int64_t col_count = schema_.count();
    for (int64_t c = 0; OB_SUCC(ret) && c < col_count; ++c) {
      FileCell cell;
      const int64_t index = c * row_count_ + cur_row_;
      if (index < all_cells_.count()) {
        cell = all_cells_.at(index);
      }
      if (OB_SUCC(ret) && OB_FAIL(row.push_back(cell))) {
      }
    }
    if (OB_SUCC(ret)) {
      ++cur_row_;
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
