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

#include "lib/container/ob_se_array.h"
#include "lib/lock/ob_spin_rwlock.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "lib/ob_define.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace share
{

// One row of __all_ai_column, cached in memory for DML rewriting and the worker.
struct ObAiColumnMeta
{
  ObAiColumnMeta()
    : table_id_(OB_INVALID_ID),
      column_id_(OB_INVALID_ID),
      max_retries_(0),
      sync_mode_(0),
      state_column_id_(OB_INVALID_ID),
      request_column_id_(OB_INVALID_ID),
      spec_column_id_(OB_INVALID_ID),
      version_column_id_(OB_INVALID_ID),
      column_name_(),
      expr_(),
      prompt_hint_() {}
  void reset()
  {
    table_id_ = OB_INVALID_ID;
    column_id_ = OB_INVALID_ID;
    max_retries_ = 0;
    sync_mode_ = 0;
    state_column_id_ = OB_INVALID_ID;
    request_column_id_ = OB_INVALID_ID;
    spec_column_id_ = OB_INVALID_ID;
    version_column_id_ = OB_INVALID_ID;
    column_name_.reset();
    expr_.reset();
    prompt_hint_.reset();
  }
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t max_retries_;
  int64_t sync_mode_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t spec_column_id_;
  uint64_t version_column_id_;
  common::ObString column_name_;
  common::ObString expr_;
  common::ObString prompt_hint_;
  TO_STRING_KV(K_(table_id), K_(column_id), K_(max_retries), K_(sync_mode),
               K_(state_column_id), K_(request_column_id), K_(spec_column_id),
               K_(version_column_id), K_(column_name), K_(expr), K_(prompt_hint));
};

class ObAiColumnMetaCache
{
public:
  static ObAiColumnMetaCache &get_instance();
  // Reload every row of __all_ai_column. Safe to call from any thread.
  int reload();
  // Copy the AI column metadata of one table into metas (deep copied using alloc).
  int get_table_metas(const uint64_t table_id,
                      common::ObIAllocator &alloc,
                      common::ObIArray<ObAiColumnMeta> &metas) const;
  // Reload if the cache is older than min_interval_us, then copy the table metadata.
  int ensure_table_metas(const uint64_t table_id,
                         const int64_t min_interval_us,
                         common::ObIAllocator &alloc,
                         common::ObIArray<ObAiColumnMeta> &metas);
  int64_t get_last_reload_ts() const { return ATOMIC_LOAD(&last_reload_ts_); }
  void reset();

private:
  ObAiColumnMetaCache();
  ~ObAiColumnMetaCache() {}
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnMetaCache);

private:
  mutable common::SpinRWLock lock_;
  common::ObArenaAllocator arena_;
  common::ObSEArray<ObAiColumnMeta *, 16> metas_;
  int64_t last_reload_ts_;
};

} // namespace share
} // namespace oceanbase
