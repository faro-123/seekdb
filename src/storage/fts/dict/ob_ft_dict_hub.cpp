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
  lib::ObMutexGuard guard(bundle_lock_);
  destroy_ik_dict_bundle();
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

int ObFTDictHub::get_ik_dict_bundle(const ObIKDictBundle *&bundle)
{
  int ret = OB_SUCCESS;
  bundle = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    const ObIKDictBundle *tmp = ATOMIC_LOAD(&ik_bundle_);
    if (OB_ISNULL(tmp)) {
      lib::ObMutexGuard guard(bundle_lock_);
      if (OB_ISNULL(tmp = ATOMIC_LOAD(&ik_bundle_))) {
        if (OB_FAIL(build_ik_dict_bundle(tmp))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      bundle = tmp;
    }
  }
  return ret;
}

int ObFTDictHub::build_ik_dict_bundle(const ObIKDictBundle *&bundle)
{
  int ret = OB_SUCCESS;
  static const ObFTDictDesc MAIN_DESC("main_dict",
                                      ObFTDictType::DICT_IK_MAIN,
                                      ObCharsetType::CHARSET_UTF8MB4,
                                      ObCollationType::CS_TYPE_UTF8MB4_BIN);
  static const ObFTDictDesc QUAN_DESC("quan_dict",
                                      ObFTDictType::DICT_IK_QUAN,
                                      ObCharsetType::CHARSET_UTF8MB4,
                                      ObCollationType::CS_TYPE_UTF8MB4_BIN);
  static const ObFTDictDesc STOP_DESC("stopword",
                                      ObFTDictType::DICT_IK_STOP,
                                      ObCharsetType::CHARSET_UTF8MB4,
                                      ObCollationType::CS_TYPE_UTF8MB4_BIN);
  ObIKDictBundle *tmp = nullptr;
  bundle = nullptr;

  if (OB_ISNULL(tmp = OB_NEWx(ObIKDictBundle, &bundle_alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(tmp->cache_main_ = OB_NEWx(ObFTCacheRangeContainer, &bundle_alloc_, bundle_alloc_))
             || OB_ISNULL(tmp->cache_quan_ = OB_NEWx(ObFTCacheRangeContainer, &bundle_alloc_, bundle_alloc_))
             || OB_ISNULL(tmp->cache_stop_ = OB_NEWx(ObFTCacheRangeContainer, &bundle_alloc_, bundle_alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(load_or_build_cache(MAIN_DESC, *tmp->cache_main_))) {
  } else if (OB_FAIL(open_range_dict(MAIN_DESC, *tmp->cache_main_, tmp->dict_main_))) {
  } else if (OB_FAIL(load_or_build_cache(QUAN_DESC, *tmp->cache_quan_))) {
  } else if (OB_FAIL(open_range_dict(QUAN_DESC, *tmp->cache_quan_, tmp->dict_quan_))) {
  } else if (OB_FAIL(load_or_build_cache(STOP_DESC, *tmp->cache_stop_))) {
  } else if (OB_FAIL(open_range_dict(STOP_DESC, *tmp->cache_stop_, tmp->dict_stop_))) {
  } else {
    ATOMIC_STORE(&ik_bundle_, tmp);
    bundle = tmp;
    LOG_INFO("succeeded to share built-in IK dictionaries", KP(tmp));
  }

  if (OB_FAIL(ret) && OB_NOT_NULL(tmp)) {
    free_ik_dict_bundle(tmp);
  }
  return ret;
}

int ObFTDictHub::load_or_build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(load_cache(desc, container))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      if (OB_FAIL(build_cache(desc, container))) {
      }
    }
  }
  return ret;
}

int ObFTDictHub::open_range_dict(const ObFTDictDesc &desc,
                                 ObFTCacheRangeContainer &container,
                                 ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  ObFTRangeDict *range_dict = nullptr;
  dict = nullptr;
  if (OB_ISNULL(range_dict = OB_NEWx(ObFTRangeDict, &bundle_alloc_, bundle_alloc_, &container, desc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(range_dict->init())) {
  } else {
    dict = range_dict;
  }
  if (OB_FAIL(ret) && OB_NOT_NULL(range_dict)) {
    OB_DELETEx(ObFTRangeDict, &bundle_alloc_, range_dict);
  }
  return ret;
}

void ObFTDictHub::destroy_ik_dict_bundle()
{
  ObIKDictBundle *bundle = const_cast<ObIKDictBundle *>(ATOMIC_LOAD(&ik_bundle_));
  ATOMIC_STORE(&ik_bundle_, static_cast<const ObIKDictBundle *>(nullptr));
  if (OB_NOT_NULL(bundle)) {
    free_ik_dict_bundle(bundle);
  }
}

void ObFTDictHub::free_ik_dict_bundle(ObIKDictBundle *bundle)
{
  if (OB_NOT_NULL(bundle)) {
    if (OB_NOT_NULL(bundle->dict_main_)) {
      bundle->dict_main_->~ObIFTDict();
    }
    if (OB_NOT_NULL(bundle->dict_quan_)) {
      bundle->dict_quan_->~ObIFTDict();
    }
    if (OB_NOT_NULL(bundle->dict_stop_)) {
      bundle->dict_stop_->~ObIFTDict();
    }
    if (OB_NOT_NULL(bundle->cache_main_)) {
      bundle->cache_main_->~ObFTCacheRangeContainer();
    }
    if (OB_NOT_NULL(bundle->cache_quan_)) {
      bundle->cache_quan_->~ObFTCacheRangeContainer();
    }
    if (OB_NOT_NULL(bundle->cache_stop_)) {
      bundle->cache_stop_->~ObFTCacheRangeContainer();
    }
    bundle->~ObIKDictBundle();
  }
  bundle_alloc_.reset();
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
