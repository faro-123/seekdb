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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObStopWordChecker hasn't been initialized", K(ret), K(inited_));
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("word is empty", K(ret), K(word));
  } else {
    common::ObString cmp_str = word.get_word().get_string();
    ObFTWord converted;
    int exist_ret = OB_SUCCESS;
    if (ObCharset::charset_type_by_coll(word.get_collation_type())
        == ObCharset::charset_type_by_coll(stopword_type_.get_collation_type())) {
      converted = ObFTWord(cmp_str.length(), cmp_str.ptr(), stopword_type_);
      exist_ret = stopword_set_.exist_refactored(converted);
    } else {
      common::ObArenaAllocator convert_allocator(lib::ObMemAttr("ChkStopWord"));
      if (OB_FAIL(common::ObCharset::charset_convert(
                      convert_allocator,
                      cmp_str,
                      word.get_collation_type(),
                      stopword_type_.get_collation_type(),
                      cmp_str))) {
      } else {
        converted = ObFTWord(cmp_str.length(), cmp_str.ptr(), stopword_type_);
        exist_ret = stopword_set_.exist_refactored(converted);
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_HASH_NOT_EXIST == exist_ret) {
        is_stopword = false;
      } else if (OB_HASH_EXIST == exist_ret) {
        is_stopword = true;
      } else if (OB_SUCCESS == exist_ret) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the exist of hastset shouldn't return success", K(ret), K(word));
      } else {
        ret = exist_ret;
        LOG_WARN("fail to do exist", K(ret), K(word));
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(word), K(word_len), K(char_cnt), K(word_freq));
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid src ft word", K(ret), K(src));
  } else if (flag_.casedown()) {
    const ObString src_str = src.get_word().get_string();
    constexpr int64_t STACK_NORMALIZE_SIZE = 256;
    const ObCharsetInfo *charset_info =
        ObCharset::get_charset(word_meta_.get_collation_type());
    if (OB_ISNULL(charset_info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (src_str.length() <= STACK_NORMALIZE_SIZE
               && 1 == charset_info->casedn_multiply) {
      char normalized_word[STACK_NORMALIZE_SIZE];
      MEMCPY(normalized_word, src_str.ptr(), src_str.length());
      const size_t normalized_len = ObCharset::casedn(
          word_meta_.get_collation_type(),
          normalized_word,
          src_str.length(),
          normalized_word,
          src_str.length());
      if (normalized_len == static_cast<size_t>(src_str.length())
          && 0 == MEMCMP(normalized_word, src_str.ptr(), src_str.length())) {
        dst = src;
      } else {
        char *dst_ptr = static_cast<char *>(allocator_.alloc(normalized_len));
        if (OB_ISNULL(dst_ptr)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(dst_ptr, normalized_word, normalized_len);
          dst = ObFTWord(normalized_len, dst_ptr, word_meta_);
        }
      }
    } else {
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(
                      word_meta_.get_collation_type(),
                      src_str,
                      dst_str,
                      allocator_))) {
      } else {
        ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
        dst = tmp;
      }
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(ft_word));
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("got null stop word checker", K(ret));
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  int64_t *word_count = nullptr;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(word), K(word_freq));
  } else if (!flag_.groupby_word()) {
    ObFTWord map_word = word;
    if (flag_.copy_word()) {
      ObString copied_word;
      if (OB_FAIL(ob_write_string(allocator_, word.get_word().get_string(), copied_word))) {
      } else {
        map_word = ObFTWord(copied_word.length(), copied_word.ptr(), word_meta_);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(word_map_.set_refactored(map_word, 1/*word count*/))) {
    }
  } else if (OB_NOT_NULL(word_count = word_map_.get(word))) {
    *word_count += word_freq;
  } else {
    ObFTWord map_word = word;
    if (flag_.copy_word()) {
      ObString copied_word;
      if (OB_FAIL(ob_write_string(allocator_, word.get_word().get_string(), copied_word))) {
      } else {
        map_word = ObFTWord(copied_word.length(), copied_word.ptr(), word_meta_);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(word_map_.set_refactored(map_word, 1/*word count*/))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
