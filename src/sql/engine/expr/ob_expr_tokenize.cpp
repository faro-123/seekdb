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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/lock/ob_mutex.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_bin.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "data_plane/fts/ob_fts_literal.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

namespace
{
// Direct-mapped process-wide cache of TOKENIZE results. The result depends only
// on the parser name, the raw properties, the collation, the dictionary
// generation and the input text, so repeated evaluations (plan reuse, prepared
// statements) skip parameter parsing, segmentation and JSON building.
struct ObTokenizeCacheEntry
{
  ObTokenizeCacheEntry()
    : hash_(0), parser_name_(nullptr), parser_name_len_(0), properties_(nullptr),
      properties_len_(0), fulltext_(nullptr), fulltext_len_(0), mode_(0),
      collation_(CS_TYPE_INVALID), dict_guard_(nullptr), value_(nullptr), value_len_(0)
  {}

  void reset()
  {
    if (nullptr != parser_name_) {
      common::ob_free(parser_name_);
      parser_name_ = nullptr;
    }
    if (nullptr != properties_) {
      common::ob_free(properties_);
      properties_ = nullptr;
    }
    if (nullptr != fulltext_) {
      common::ob_free(fulltext_);
      fulltext_ = nullptr;
    }
    if (nullptr != value_) {
      common::ob_free(value_);
      value_ = nullptr;
    }
    hash_ = 0;
    parser_name_len_ = 0;
    properties_len_ = 0;
    fulltext_len_ = 0;
    value_len_ = 0;
    mode_ = 0;
    collation_ = CS_TYPE_INVALID;
    dict_guard_ = nullptr;
  }

  uint64_t hash_;
  char *parser_name_;
  int32_t parser_name_len_;
  char *properties_;
  int32_t properties_len_;
  char *fulltext_;
  int32_t fulltext_len_;
  int64_t mode_;
  ObCollationType collation_;
  const void *dict_guard_;
  char *value_;
  int32_t value_len_;
};

class ObTokenizeResultCache
{
public:
  static const int64_t CACHE_SLOTS = 64;
  static const int64_t MAX_TEXT_LEN = 8 * 1024;
  static const int64_t MAX_VALUE_LEN = 16 * 1024;

  static ObTokenizeResultCache *get_instance()
  {
    static ObTokenizeResultCache cache;
    return &cache;
  }

  static uint64_t calc_hash(const common::ObString &parser_name,
                            const common::ObString &properties,
                            const common::ObString &fulltext,
                            const int64_t mode,
                            const ObCollationType collation,
                            const void *dict_guard)
  {
    uint64_t hash = 1469598103934665603ULL;
    hash = mix(hash, parser_name.ptr(), parser_name.length());
    hash = mix(hash, properties.ptr(), properties.length());
    hash = mix(hash, fulltext.ptr(), fulltext.length());
    hash = mix(hash, reinterpret_cast<const char *>(&mode), sizeof(mode));
    hash = mix(hash, reinterpret_cast<const char *>(&collation), sizeof(collation));
    hash = mix(hash, reinterpret_cast<const char *>(&dict_guard), sizeof(dict_guard));
    return hash;
  }

  bool lookup(const uint64_t hash,
              const common::ObString &parser_name,
              const common::ObString &properties,
              const common::ObString &fulltext,
              const int64_t mode,
              const ObCollationType collation,
              const void *dict_guard,
              common::ObString &value,
              common::ObIAllocator &allocator)
  {
    bool hit = false;
    lib::ObMutexGuard guard(mutex_);
    ObTokenizeCacheEntry &entry = slots_[hash % CACHE_SLOTS];
    if (entry.hash_ == hash && entry.mode_ == mode && entry.collation_ == collation
        && entry.dict_guard_ == dict_guard
        && match(entry.parser_name_, entry.parser_name_len_, parser_name)
        && match(entry.properties_, entry.properties_len_, properties)
        && match(entry.fulltext_, entry.fulltext_len_, fulltext)) {
      char *buf = static_cast<char *>(allocator.alloc(entry.value_len_));
      if (nullptr != buf) {
        MEMCPY(buf, entry.value_, entry.value_len_);
        value.assign_ptr(buf, entry.value_len_);
        hit = true;
      }
    }
    return hit;
  }

  void insert(const uint64_t hash,
              const common::ObString &parser_name,
              const common::ObString &properties,
              const common::ObString &fulltext,
              const int64_t mode,
              const ObCollationType collation,
              const void *dict_guard,
              const common::ObString &value)
  {
    if (fulltext.length() > MAX_TEXT_LEN || value.length() > MAX_VALUE_LEN) {
      return;
    }
    lib::ObMutexGuard guard(mutex_);
    char *parser_buf = copy_string(parser_name);
    char *properties_buf = copy_string(properties);
    char *fulltext_buf = copy_string(fulltext);
    char *value_buf = copy_string(value);
    if (nullptr != parser_buf && nullptr != properties_buf
        && nullptr != fulltext_buf && nullptr != value_buf) {
      ObTokenizeCacheEntry &entry = slots_[hash % CACHE_SLOTS];
      entry.reset();
      entry.hash_ = hash;
      entry.parser_name_ = parser_buf;
      entry.parser_name_len_ = parser_name.length();
      entry.properties_ = properties_buf;
      entry.properties_len_ = properties.length();
      entry.fulltext_ = fulltext_buf;
      entry.fulltext_len_ = fulltext.length();
      entry.mode_ = mode;
      entry.collation_ = collation;
      entry.dict_guard_ = dict_guard;
      entry.value_ = value_buf;
      entry.value_len_ = value.length();
    } else {
      if (nullptr != parser_buf) { common::ob_free(parser_buf); }
      if (nullptr != properties_buf) { common::ob_free(properties_buf); }
      if (nullptr != fulltext_buf) { common::ob_free(fulltext_buf); }
      if (nullptr != value_buf) { common::ob_free(value_buf); }
    }
  }

private:
  static uint64_t mix(uint64_t hash, const char *ptr, const int64_t len)
  {
    for (int64_t i = 0; i < len; ++i) {
      hash ^= static_cast<uint8_t>(ptr[i]);
      hash *= 1099511628211ULL;
    }
    return hash;
  }

  static bool match(const char *cached, const int32_t cached_len, const common::ObString &value)
  {
    return cached_len == value.length()
        && (0 == value.length() || 0 == MEMCMP(cached, value.ptr(), value.length()));
  }

  static char *copy_string(const common::ObString &value)
  {
    char *buf = static_cast<char *>(common::ob_malloc(
        value.length() > 0 ? value.length() : 1, common::ObMemAttr("TokResultCache")));
    if (nullptr != buf && value.length() > 0) {
      MEMCPY(buf, value.ptr(), value.length());
    }
    return buf;
  }

  lib::ObMutex mutex_;
  ObTokenizeCacheEntry slots_[CACHE_SLOTS];
};
} // namespace

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObTokenizeResultCache *cache = ObTokenizeResultCache::get_instance();
    const void *dict_guard = static_cast<const void *>(
        storage::ObFTParseData::instance().stop_word_checker());
    ObString raw_text;
    ObString raw_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    ObString raw_props;
    const ObCollationType raw_collation = expr.args_[0]->obj_meta_.get_collation_type();
    uint64_t cache_hash = 0;
    ObString cached_value;
    bool cache_hit = false;
    bool cacheable = (nullptr != cache && !expr.args_[0]->obj_meta_.is_lob_storage());
    if (cacheable) {
      ObDatum *text_datum = nullptr;
      ObDatum *name_datum = nullptr;
      ObDatum *props_datum = nullptr;
      if (OB_FAIL(expr.args_[0]->eval(ctx, text_datum))) {
      } else if (OB_ISNULL(text_datum) || text_datum->is_null()) {
        cacheable = false;
      } else if (FALSE_IT(raw_text = text_datum->get_string())) {
      } else if (expr.arg_cnt_ >= 2) {
        if (OB_FAIL(expr.args_[1]->eval(ctx, name_datum))) {
        } else if (OB_ISNULL(name_datum) || name_datum->is_null()
                   || !ob_is_string_type(expr.args_[1]->datum_meta_.type_)) {
          cacheable = false;
        } else {
          raw_name = name_datum->get_string();
        }
      }
      if (OB_SUCC(ret) && cacheable && expr.arg_cnt_ >= 3) {
        if (OB_FAIL(expr.args_[2]->eval(ctx, props_datum))) {
        } else if (OB_ISNULL(props_datum) || props_datum->is_null()
                   || !ob_is_string_type(expr.args_[2]->datum_meta_.type_)) {
          cacheable = false;
        } else {
          raw_props = props_datum->get_string();
        }
      }
      if (OB_SUCC(ret) && cacheable) {
        cache_hash = ObTokenizeResultCache::calc_hash(raw_name, raw_props, raw_text,
                                                      0, raw_collation, dict_guard);
        cache_hit = cache->lookup(cache_hash, raw_name, raw_props, raw_text,
                                  0, raw_collation, dict_guard,
                                  cached_value, temp_allocator);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (cache_hit) {
      if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, cached_value))) {
      }
    } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
    } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
    } else {
      bool should_pack_diff = false;
      ObString raw_binary;
      if (OB_ISNULL(json_result)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (json_result->is_bin()) {
        ObJsonBin *json_bin = static_cast<ObJsonBin *>(json_result);
        ret = json_bin->should_pack_diff(should_pack_diff);
      }
      if (OB_FAIL(ret)) {
      } else if (should_pack_diff) {
        ret = ObJsonExprHelper::pack_json_res(expr, ctx, temp_allocator, json_result, expr_datum);
      } else if (OB_FAIL(share::ObJsonWrapper::get_raw_binary(json_result, raw_binary, &temp_allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, raw_binary))) {
      } else if (cacheable) {
        cache->insert(cache_hash, raw_name, raw_props, raw_text, 0, raw_collation,
                      dict_guard, raw_binary);
      }
    }
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (!param.properties_.empty()
             && OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  // TOKENIZE receives the built-in parser name without a version in the
  // hot path.  The built-ins all use version 1, so avoid constructing and
  // serializing a temporary parser object for this common case.
  static const char *BUILTIN_PARSER_NAMES[] = {
      storage::ObFTSLiteral::PARSER_NAME_SPACE,
      storage::ObFTSLiteral::PARSER_NAME_NGRAM,
      storage::ObFTSLiteral::PARSER_NAME_BENG,
      storage::ObFTSLiteral::PARSER_NAME_IK,
      storage::ObFTSLiteral::PARSER_NAME_NGRAM2,
  };
  bool is_builtin_name = false;
  for (const char *name : BUILTIN_PARSER_NAMES) {
    if (0 == input_str.case_compare(ObString::make_string(name))) {
      const int64_t name_len = STRLEN(name);
      MEMCPY(param.parser_name_buf_, name, name_len);
      param.parser_name_buf_[name_len] = '.';
      param.parser_name_buf_[name_len + 1] = '1';
      param.parser_name_buf_[name_len + 2] = '\0';
      param.parser_name_ = ObString(name_len + 2, param.parser_name_buf_);
      is_builtin_name = true;
      break;
    }
  }
  if (is_builtin_name) {
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(param.parser_name_buf_, sizeof(param.parser_name_buf_)))) {
  } else {
    param.parser_name_ = ObString::make_string(param.parser_name_buf_);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
