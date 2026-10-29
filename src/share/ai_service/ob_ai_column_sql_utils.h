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

#pragma once

#include "lib/string/ob_sql_string.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace share
{

// Use the same SQL representation in DML, retry, and the background worker.
// Hex strings do not depend on NO_BACKSLASH_ESCAPES and preserve binary keys.
inline int append_ai_hex_literal(common::ObSqlString &sql, const common::ObString &value)
{
  int ret = sql.append("X'");
  static const char hex[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value.ptr()[i]);
    const char pair[2] = {hex[c >> 4], hex[c & 15]};
    if (OB_FAIL(sql.append(pair, 2))) {}
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}

inline int append_ai_rowkey_value(common::ObSqlString &sql, const common::ObObj &value,
                                  common::ObIAllocator &alloc,
                                  const common::ObObjPrintParams &print_params)
{
  int ret = OB_SUCCESS;
  if (value.is_null()) {
    ret = OB_ERR_NULL_VALUE;
  } else if (value.is_string_type()) {
    ret = append_ai_hex_literal(sql, value.get_string());
  } else {
    char *buf = NULL;
    int64_t len = 0;
    int64_t pos = 0;
    if (OB_FAIL(value.print_sql_literal(buf, len, pos, alloc, print_params))) {
    } else if (OB_FAIL(sql.append(buf, pos))) {
    }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
