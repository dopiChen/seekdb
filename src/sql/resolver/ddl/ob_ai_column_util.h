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

// AI COLUMN (asynchronous generated columns backed by AI_COMPLETE / AI_EMBED).
//
// Storage model of one AI column `c` of table t:
//   c                 user visible result column, NULL until READY
//   __ai_state_c      hidden, 0 PENDING / 1 READY / 2 FAILED
//   __ai_req_c        hidden, id of the request that may publish into c
//   __ai_spec_c       hidden, definition version; its column comment holds the
//                     encoded ObAIColumnDef, so DML and the worker read the
//                     definition from the table schema (atomic with the DDL)
// oceanbase.__all_ai_column mirrors the definitions, oceanbase.__all_ai_column_task
// holds the pending requests and is written in the business transaction.

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace common
{
class ObISQLClient;
}
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;
}
}
namespace sql
{

enum ObAIColumnFunc : int32_t
{
  AI_COLUMN_FUNC_INVALID = 0,
  AI_COLUMN_FUNC_COMPLETE = 1,
  AI_COLUMN_FUNC_EMBED = 2,
};

// value of the hidden state column
enum ObAIColumnState : int32_t
{
  AI_COLUMN_STATE_PENDING = 0,
  AI_COLUMN_STATE_READY = 1,
  AI_COLUMN_STATE_FAILED = 2,
};

// value of __all_ai_column_task.state
enum ObAIColumnTaskState : int32_t
{
  AI_TASK_STATE_PENDING = 0,
  AI_TASK_STATE_RUNNING = 1,
  AI_TASK_STATE_RETRY_WAIT = 3,
};

struct ObAIColumnDef
{
  ObAIColumnDef() { reset(); }
  void reset();
  int encode(common::ObSqlString &out) const;
  int decode(const common::ObString &in, common::ObIAllocator &allocator);
  // e.g. AI_COMPLETE('m', concat('x', content)) WITH SYSTEM PROMPT 'Return ...'
  int build_normalized_expr(common::ObSqlString &out) const;
  bool depends_on(const common::ObString &column_name) const;
  TO_STRING_KV(K_(column_name), K_(func), K_(model), K_(input_expr), K_(extra_arg), K_(deps),
               K_(max_retries), K_(sync_mode), K_(result_type), K_(dimension), K_(version),
               K_(system_prompt), K_(column_id), K_(state_column_id), K_(request_column_id),
               K_(spec_column_id));

  common::ObString column_name_;
  int32_t func_;
  common::ObString model_;
  common::ObString input_expr_;     // SQL text of the model input argument
  common::ObString extra_arg_;      // SQL text of the optional third argument
  common::ObSEArray<common::ObString, 4> deps_;  // columns referenced by the input
  int64_t max_retries_;
  common::ObString sync_mode_;
  common::ObString result_type_;    // printable result column type
  int64_t dimension_;               // VECTOR(n) result columns
  int64_t version_;
  common::ObString system_prompt_;  // type hint sent as the system message
  // column ids, filled from the table schema
  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t spec_column_id_;
};

class ObAIColumnUtil
{
public:
  static constexpr const char *STATE_PREFIX = "__ai_state_";
  static constexpr const char *REQUEST_PREFIX = "__ai_req_";
  static constexpr const char *SPEC_PREFIX = "__ai_spec_";
  static constexpr int64_t DEFAULT_MAX_RETRIES = 3;
  static constexpr int64_t MAX_MAX_RETRIES = 100;

  // resolve "AI COLUMN (expr) WITH (options)" of result column `column`
  static int resolve_spec(const ParseNode &spec_node, const share::schema::ObColumnSchemaV2 &column,
                          common::ObIAllocator &allocator, ObAIColumnDef &def);
  // the type hint added to the system prompt for a result column type
  static int get_type_hint(const share::schema::ObColumnSchemaV2 &column, common::ObString &hint);
  // check that the input only references existing, non AI columns
  static int check_dependencies(const share::schema::ObTableSchema &table_schema,
                                const common::ObIArray<ObAIColumnDef> &new_defs);
  // append the three hidden columns of def to the table schema
  static int add_hidden_columns(share::schema::ObTableSchema &table_schema, ObAIColumnDef &def,
                                uint64_t state_column_id, uint64_t request_column_id,
                                uint64_t spec_column_id);
  static int build_hidden_column(const ObAIColumnDef &def, const char *prefix, uint64_t table_id,
                                 uint64_t column_id, share::schema::ObColumnSchemaV2 &column);
  static bool is_ai_hidden_column_name(const common::ObString &name);
  // AI columns of a table, read from the hidden spec columns
  static int get_ai_columns(const share::schema::ObTableSchema &table_schema,
                            common::ObIAllocator &allocator, common::ObIArray<ObAIColumnDef> &defs);
  static bool has_ai_column(const share::schema::ObTableSchema &table_schema);
  // AI column whose result, state, request or spec column is column_id (or nullptr)
  static const ObAIColumnDef *find_by_any_column_id(const common::ObIArray<ObAIColumnDef> &defs,
                                                    uint64_t column_id);
  // unique, strictly increasing request id (time based, survives restarts)
  static int64_t gen_request_id();
  // ALTER TABLE ... RETRY AI COLUMN: new requests for the rows matching where
  static int retry_rows(uint64_t table_id, const common::ObString &column_name,
                        const common::ObString &where);
  static int get_full_table_name(const share::schema::ObTableSchema &table_schema, common::ObSqlString &name);
  // OB_AI_COLUMN_NOT_READY unless every row of the column is READY
  static int check_column_ready(const share::schema::ObTableSchema &table_schema, const ObAIColumnDef &def);
  // rewrite oceanbase.__all_ai_column rows of a table from its schema
  static int sync_metadata(const share::schema::ObTableSchema &table_schema,
                           common::ObISQLClient &sql_client);
  static int remove_metadata(uint64_t table_id, common::ObISQLClient &sql_client);
  // after CREATE / ALTER TABLE: wait for the new schema and mirror its AI columns
  static int sync_metadata_after_ddl(const common::ObString &database_name,
                                     const common::ObString &table_name,
                                     const int64_t min_schema_version);
  // SQL string literal with quotes escaped
  static int append_sql_string(common::ObSqlString &sql, const common::ObString &str);
  static int append_ident(common::ObSqlString &sql, const common::ObString &name);
};

} // namespace sql
} // namespace oceanbase
