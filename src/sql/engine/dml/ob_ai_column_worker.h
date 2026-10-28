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

// Background worker of AI COLUMN tasks (oceanbase.__all_ai_column_task).
//
// A task group is identified by (table_id, request_id): the AI columns of one
// row change. A worker thread claims a group (state -> RUNNING), reads the
// model inputs of the row, calls the models, and
//   - when every column reached a terminal result: publishes all results and
//     states in one transaction, guarded by the request id stored in the row
//     (late results of superseded requests and deleted rows update nothing),
//     and removes the tasks;
//   - otherwise keeps successful results in the task rows and schedules a
//     retry with backoff (state RETRY_WAIT, retry_count + 1).
// RUNNING tasks left by a crash are made PENDING again at startup.

#include "lib/thread/threads.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "sql/resolver/ddl/ob_ai_column_util.h"

namespace oceanbase
{
namespace sql
{

class ObAIColumnWorker : public lib::Threads
{
public:
  static constexpr int64_t WORKER_THREAD_COUNT = 4;
  static constexpr int64_t IDLE_SLEEP_US = 100 * 1000;
  static constexpr int64_t BASE_RETRY_DELAY_US = 200 * 1000;
  static constexpr int64_t MAX_RETRY_DELAY_US = 10 * 1000 * 1000;
  static constexpr int64_t SWEEP_INTERVAL_US = 2 * 1000 * 1000;

  static ObAIColumnWorker &get_instance();
  int init();
  virtual int start() override;
  virtual void stop() override;
  virtual void wait() override;
  virtual void run(int64_t idx) override;

  struct TaskCol
  {
    TaskCol() : column_id_(common::OB_INVALID_ID), retry_count_(0), version_(0), cached_(false),
                def_(nullptr), live_(false), ok_(false), terminal_(false), attempted_(false) {}
    uint64_t column_id_;
    int64_t retry_count_;
    int64_t version_;
    bool cached_;                 // success kept from a previous attempt
    common::ObString cached_value_;
    const ObAIColumnDef *def_;
    bool live_;                   // request still current for the row
    bool ok_;
    bool terminal_;
    bool attempted_;             // a failed attempt consumes this column's retry budget
    common::ObString literal_;    // SQL literal published into the result column
    common::ObString error_;
    TO_STRING_KV(K_(column_id), K_(retry_count), K_(cached), K_(live), K_(ok), K_(terminal));
  };

private:
  ObAIColumnWorker() : lib::Threads(WORKER_THREAD_COUNT), is_inited_(false), recovered_(false) {}
  bool is_service_ready() const;
  int recover_running_tasks();
  void sweep_dropped_tables();
  int claim_group(uint64_t &table_id, int64_t &request_id, bool &claimed);
  int process_group(uint64_t table_id, int64_t request_id);
  int call_model(const ObAIColumnDef &def, const common::ObString &input,
                 const common::ObString &extra_arg, common::ObIAllocator &allocator,
                 common::ObString &raw_result, common::ObSqlString &error);
  int to_literal(const share::schema::ObColumnSchemaV2 &column, const ObAIColumnDef &def,
                 const common::ObString &raw, common::ObIAllocator &allocator,
                 common::ObString &literal, common::ObSqlString &error);
  int publish(uint64_t table_id, int64_t request_id, const common::ObString &full_table_name,
              const common::ObString &predicate, common::ObIArray<TaskCol> &cols);
  int schedule_retry(uint64_t table_id, int64_t request_id, common::ObIArray<TaskCol> &cols);
  int drop_group(uint64_t table_id, int64_t request_id);
  bool is_inited_;
  bool recovered_;
  int64_t last_sweep_ts_ = 0;
};

} // namespace sql
} // namespace oceanbase
