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

// Row level AI COLUMN maintenance inside business DML (see ob_ai_column_util.h).
//
// before the row is written:
//   INSERT: result := NULL, state := PENDING, request := new id
//   UPDATE: when an input column or the primary key changed, the same as INSERT
//           (the resolver added the assignments), otherwise the old values stay
// after the row is written (nested SQL in the same transaction):
//   INSERT / invalidating UPDATE: add tasks, drop tasks of the old row
//   DELETE: drop the tasks of the row
// Sessions of the AI worker (inner sessions) publish results and are skipped.

#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_dml_service.h"

namespace oceanbase
{
namespace sql
{
class ObTableModifyOp;

class ObAIColumnDmlHandle
{
public:
  static int before_insert(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef);
  static int before_update(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef);
  static int after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef, ObDmlEventType event);
  static bool need_maintain(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef);
private:
  static int gen_predicate(ObTableModifyOp &op, const ObAIColumnDmlArg &arg, const ExprFixedArray &row,
                           common::ObIAllocator &alloc, common::ObString &pred);
  static int get_int(ObTableModifyOp &op, const ExprFixedArray &row, int32_t idx, bool &is_null,
                     int64_t &value);
  static int execute_nested(ObTableModifyOp &op, const char *sql);
};

} // namespace sql
} // namespace oceanbase
