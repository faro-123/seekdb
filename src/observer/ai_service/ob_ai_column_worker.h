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

#include "lib/task/ob_timer.h"
#include "lib/container/ob_se_array.h"
#include "share/ai_service/ob_ai_column_meta.h"
#include "lib/string/ob_string.h"
#include "share/ob_define.h"

namespace oceanbase
{
namespace observer
{

// Periodically drains __all_ai_column_task: evaluates the stored AI expression for one
// row (all AI columns of that row at once), writes the results back and removes tasks.
class ObAiColumnWorker : public common::ObTimerTask
{
public:
  static ObAiColumnWorker &get_instance();
  int start();
  void stop();
  virtual void runTimerTask() override;

private:
  struct AiTask
  {
    AiTask() : table_id_(OB_INVALID_ID), column_id_(OB_INVALID_ID), retry_count_(0),
               state_(0), rowkey_(), request_id_() {}
    uint64_t table_id_;
    uint64_t column_id_;
    int64_t retry_count_;
    int64_t state_;
    common::ObString rowkey_;
    common::ObString request_id_;
  };

  ObAiColumnWorker() : is_start_(false) {}
  int fetch_tasks(const common::ObIAllocator &unused, common::ObIArray<AiTask> &tasks);
  int process_task_group(const uint64_t table_id,
                         const common::ObString &rowkey,
                         const common::ObIArray<AiTask> &group);
  int handle_task_failure(const uint64_t table_id,
                          const common::ObString &rowkey,
                          const common::ObIArray<AiTask> &group,
                          const common::ObIArray<share::ObAiColumnMeta> &metas);
  int delete_tasks(const uint64_t table_id,
                   const common::ObString &rowkey,
                   const common::ObIArray<AiTask> &group);
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorker);

private:
  bool is_start_;
};

} // namespace observer
} // namespace oceanbase
