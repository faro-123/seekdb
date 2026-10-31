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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/atomic/ob_atomic.h"
#include "lib/ob_errno.h"
#include "lib/alloc/ob_malloc_allocator.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
namespace
{
int get_builtin_dict_index(const ObFTDictType type, int64_t &index)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case ObFTDictType::DICT_IK_MAIN:
      index = 0;
      break;
    case ObFTDictType::DICT_IK_QUAN:
      index = 1;
      break;
    case ObFTDictType::DICT_IK_STOP:
      index = 2;
      break;
    default:
      ret = OB_NOT_SUPPORTED;
      break;
  }
  return ret;
}
}

int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    cache_allocator_ = lib::ObMallocAllocator::get_instance();
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  is_inited_ = false;
  // Range dictionaries borrow the cache handles, so destroy the wrappers
  // before releasing the containers that pin their DAT blocks.
  if (OB_NOT_NULL(cache_allocator_)) {
    for (int64_t i = 0; i < 3; ++i) {
      if (OB_NOT_NULL(builtin_dicts_[i])) {
        builtin_dicts_[i]->~ObFTRangeDict();
        cache_allocator_->free(builtin_dicts_[i]);
        builtin_dicts_[i] = nullptr;
      }
      if (OB_NOT_NULL(builtin_containers_[i])) {
        builtin_containers_[i]->~ObFTCacheRangeContainer();
        cache_allocator_->free(builtin_containers_[i]);
        builtin_containers_[i] = nullptr;
      }
    }
  }
  cache_allocator_ = nullptr;
  return ret;
}

int ObFTDictHub::get_builtin_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  dict = nullptr;
  int64_t index = -1;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(get_builtin_dict_index(desc.type_, index))) {
  } else if (desc.charset_ != ObCharsetType::CHARSET_UTF8MB4
             || desc.coll_type_ != ObCollationType::CS_TYPE_UTF8MB4_BIN) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_NOT_NULL(builtin_dicts_[index])) {
        dict = builtin_dicts_[index];
      }
    }
    if (OB_ISNULL(dict)) {
      ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_NOT_NULL(builtin_dicts_[index])) {
        dict = builtin_dicts_[index];
      } else {
        ObFTCacheRangeContainer *container = nullptr;
        ObFTRangeDict *range_dict = nullptr;
        ObFTDictInfo info;
        if (OB_ISNULL(container = OB_NEWx(ObFTCacheRangeContainer,
                                          cache_allocator_,
                                          *cache_allocator_))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, *container))) {
        } else if (OB_ISNULL(range_dict = OB_NEWx(ObFTRangeDict,
                                                  cache_allocator_,
                                                  *cache_allocator_,
                                                  container,
                                                  desc))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(range_dict->init())) {
        } else {
          info.range_count_ = static_cast<int32_t>(container->get_handles().size());
          if (OB_FAIL(put_dict_info(key, info))) {
          } else {
            builtin_containers_[index] = container;
            builtin_dicts_[index] = range_dict;
            dict = range_dict;
            container = nullptr;
            range_dict = nullptr;
          }
        }
        if (OB_NOT_NULL(range_dict)) {
          range_dict->~ObFTRangeDict();
          cache_allocator_->free(range_dict);
        }
        if (OB_NOT_NULL(container)) {
          container->~ObFTCacheRangeContainer();
          cache_allocator_->free(container);
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
