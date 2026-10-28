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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/resolver/ddl/ob_ai_column_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

// Request ids generated before the rows are written, by row predicate. In multi-row
// statements the after-row loop restores each row's key columns but not the values set
// by before_insert/before_update, so the ids are looked up here. One statement runs
// its DML operator on one thread; the registry belongs to the operator being executed.
struct AIRowRequest
{
  common::ObString pred_;
  int64_t request_id_;
  common::ObString invalidated_;   // one byte per AI column, without a 64-column limit
};

struct AIRowRequestRegistry
{
  const void *op_ = nullptr;
  int64_t stmt_ts_ = 0;
  common::ObSEArray<AIRowRequest, 16> rows_;
  void bind(ObTableModifyOp &op)
  {
    const ObSQLSessionInfo *session = op.get_exec_ctx().get_my_session();
    const int64_t ts = OB_NOT_NULL(session) ? session->get_query_start_time() : 0;
    if (op_ != &op || stmt_ts_ != ts) {
      op_ = &op;
      stmt_ts_ = ts;
      rows_.reset();
    }
  }
};

static thread_local AIRowRequestRegistry ai_row_requests;

static int record_row_request(ObTableModifyOp &op, const ObString &pred, int64_t request_id,
                              const ObString &invalidated)
{
  int ret = OB_SUCCESS;
  AIRowRequest row;
  ai_row_requests.bind(op);
  if (OB_FAIL(ob_write_string(op.get_exec_ctx().get_allocator(), pred, row.pred_))) {
  } else if (OB_FAIL(ob_write_string(op.get_exec_ctx().get_allocator(), invalidated, row.invalidated_))) {
  } else {
    row.request_id_ = request_id;
    // a later change of the same row in the statement wins
    for (int64_t i = ai_row_requests.rows_.count() - 1; i >= 0; --i) {
      if (ai_row_requests.rows_.at(i).pred_ == row.pred_) {
        ai_row_requests.rows_.at(i) = row;
        return ret;
      }
    }
    ret = ai_row_requests.rows_.push_back(row);
  }
  return ret;
}

static const AIRowRequest *find_row_request(ObTableModifyOp &op, const ObString &pred)
{
  const AIRowRequest *found = nullptr;
  ai_row_requests.bind(op);
  for (int64_t i = ai_row_requests.rows_.count() - 1; nullptr == found && i >= 0; --i) {
    if (ai_row_requests.rows_.at(i).pred_ == pred) {
      found = &ai_row_requests.rows_.at(i);
    }
  }
  return found;
}

bool ObAIColumnDmlHandle::need_maintain(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef)
{
  const ObSQLSessionInfo *session = op.get_exec_ctx().get_my_session();
  // the AI worker publishes results through inner sessions
  return ctdef.is_primary_index_ && ctdef.ai_arg_.is_valid() && OB_NOT_NULL(session)
         && !session->is_inner();
}

int ObAIColumnDmlHandle::get_int(ObTableModifyOp &op, const ExprFixedArray &row, int32_t idx,
                                 bool &is_null, int64_t &value)
{
  int ret = OB_SUCCESS;
  ObDatum *datum = nullptr;
  is_null = true;
  value = 0;
  if (idx < 0 || idx >= row.count() || OB_ISNULL(row.at(idx))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(row.at(idx)->eval(op.get_eval_ctx(), datum))) {
  } else if (!datum->is_null()) {
    is_null = false;
    value = datum->get_int();
  }
  return ret;
}

static void set_int_datum(ObTableModifyOp &op, const ExprFixedArray &row, int32_t idx, int64_t value)
{
  ObExpr *expr = row.at(idx);
  ObDatum &datum = expr->locate_datum_for_write(op.get_eval_ctx());
  datum.set_int(value);
  expr->set_evaluated_projected(op.get_eval_ctx());
}

static void set_null_datum(ObTableModifyOp &op, const ExprFixedArray &row, int32_t idx)
{
  ObExpr *expr = row.at(idx);
  ObDatum &datum = expr->locate_datum_for_write(op.get_eval_ctx());
  datum.set_null();
  expr->set_evaluated_projected(op.get_eval_ctx());
}

int ObAIColumnDmlHandle::before_insert(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef)
{
  int ret = OB_SUCCESS;
  if (need_maintain(op, ctdef)) {
    const ObAIColumnDmlArg &arg = ctdef.ai_arg_;
    const ExprFixedArray &row = ctdef.new_row_;
    const int64_t request_id = ObAIColumnUtil::gen_request_id();
    ObArenaAllocator alloc("AIColIns");
    ObString pred;
    ObSqlString invalidated;
    for (int64_t i = 0; OB_SUCC(ret) && i < arg.cols_.count(); ++i) {
      const ObAIColumnDmlCol &col = arg.cols_.at(i);
      ObDatum *result = nullptr;
      if (col.result_idx_ < 0 || col.state_idx_ < 0 || col.req_idx_ < 0
          || col.result_idx_ >= row.count() || col.state_idx_ >= row.count() || col.req_idx_ >= row.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid AI column row layout", K(ret), K(col), K(row.count()));
      } else if (OB_FAIL(row.at(col.result_idx_)->eval(op.get_eval_ctx(), result))) {
      } else if (!result->is_null()) {
        // results are produced by the model only
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "assigning a value to an AI COLUMN");
      } else {
        set_null_datum(op, row, col.result_idx_);
        set_int_datum(op, row, col.state_idx_, AI_COLUMN_STATE_PENDING);
        // the AI columns of one row change share a request id (published together)
        set_int_datum(op, row, col.req_idx_, request_id);
        ret = invalidated.append("1");
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(gen_predicate(op, arg, row, alloc, pred))) {
        if (OB_ERR_NULL_VALUE == ret) {
          ret = OB_SUCCESS;   // keys are generated later: fall back to the row values
        }
      } else {
        ret = record_row_request(op, pred, request_id, ObString(invalidated.length(), invalidated.ptr()));
      }
    }
  }
  return ret;
}

int ObAIColumnDmlHandle::before_update(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef)
{
  int ret = OB_SUCCESS;
  if (need_maintain(op, ctdef)) {
    const ObAIColumnDmlArg &arg = ctdef.ai_arg_;
    const ExprFixedArray &old_row = ctdef.old_row_;
    const ExprFixedArray &new_row = ctdef.new_row_;
    ObEvalCtx &eval_ctx = op.get_eval_ctx();
    // primary key change re-generates every AI column of the row
    bool pk_changed = false;
    for (int64_t i = 0; OB_SUCC(ret) && !pk_changed && i < arg.pk_cols_.count(); ++i) {
      const int32_t idx = arg.pk_cols_.at(i).idx_;
      ObDatum *o = nullptr;
      ObDatum *n = nullptr;
      if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, o))) {
      } else if (OB_FAIL(new_row.at(idx)->eval(eval_ctx, n))) {
      } else {
        pk_changed = !ObDatum::binary_equal(*o, *n);
      }
    }
    int64_t request_id = 0;
    ObSqlString invalidated;
    bool any_invalidated = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < arg.cols_.count(); ++i) {
      const ObAIColumnDmlCol &col = arg.cols_.at(i);
      bool invalidate = pk_changed;
      for (int64_t d = 0; OB_SUCC(ret) && !invalidate && d < col.dep_cnt_; ++d) {
        const int32_t idx = arg.dep_idxs_.at(col.dep_begin_ + d);
        ObDatum *o = nullptr;
        ObDatum *n = nullptr;
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, o))) {
        } else if (OB_FAIL(new_row.at(idx)->eval(eval_ctx, n))) {
        } else {
          invalidate = !ObDatum::binary_equal(*o, *n);
        }
      }
      if (OB_FAIL(ret)) {
      } else if (invalidate) {
        if (0 == request_id) {
          request_id = ObAIColumnUtil::gen_request_id();
        }
        set_null_datum(op, new_row, col.result_idx_);
        set_int_datum(op, new_row, col.state_idx_, AI_COLUMN_STATE_PENDING);
        set_int_datum(op, new_row, col.req_idx_, request_id);
        any_invalidated = true;
      } else {
        // input unchanged: keep result, state and request of the old row
        const int32_t idxs[3] = {col.result_idx_, col.state_idx_, col.req_idx_};
        for (int64_t k = 0; OB_SUCC(ret) && k < 3; ++k) {
          ObDatum *o = nullptr;
          ObExpr *expr = new_row.at(idxs[k]);
          if (expr == old_row.at(idxs[k])) {
            // not assigned by the statement: the new row already holds the old value
          } else if (OB_FAIL(old_row.at(idxs[k])->eval(eval_ctx, o))) {
          } else {
            // copy before locate_datum_for_write() points the target at its own buffer
            const ObDatum old_value = *o;
            ObDatum &target = expr->locate_datum_for_write(eval_ctx);
            if (old_value.is_null()) {
              target.set_null();
            } else if (OB_FAIL(target.deep_copy(old_value, op.get_exec_ctx().get_allocator()))) {
            }
            expr->set_evaluated_projected(eval_ctx);
          }
        }
      }
      if (OB_SUCC(ret)) {
        ret = invalidated.append(invalidate ? "1" : "0");
      }
    }
    if (OB_SUCC(ret) && any_invalidated) {
      ObArenaAllocator alloc("AIColUpd");
      ObString pred;
      if (OB_FAIL(gen_predicate(op, arg, new_row, alloc, pred))) {
        if (OB_ERR_NULL_VALUE == ret) {
          ret = OB_SUCCESS;
        }
      } else {
        ret = record_row_request(op, pred, request_id, ObString(invalidated.length(), invalidated.ptr()));
      }
    }
  }
  return ret;
}

int ObAIColumnDmlHandle::gen_predicate(ObTableModifyOp &op, const ObAIColumnDmlArg &arg,
                                       const ExprFixedArray &row, ObIAllocator &alloc, ObString &pred)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  int64_t len = 0;
  int64_t pos = 0;
  if (OB_FAIL(ForeignKeyHandle::gen_where(op.get_eval_ctx(), buf, len, pos, arg.pk_cols_, row, alloc,
                                          op.get_obj_print_params()))) {
    LOG_WARN("failed to build AI task row predicate", K(ret));
  } else {
    // pos counts the terminating NUL
    pred.assign_ptr(buf, static_cast<int32_t>(pos > 0 ? pos - 1 : 0));
  }
  return ret;
}

int ObAIColumnDmlHandle::execute_nested(ObTableModifyOp &op, const char *sql)
{
  int ret = OB_SUCCESS;
  // like foreign key cascades: the task statement runs as nested SQL of this statement
  // (same transaction, no statement start/end of its own)
  ObDASCtx &das_ctx = op.get_exec_ctx().get_das_ctx();
  const bool save_fk_cascading = das_ctx.is_fk_cascading_;
  das_ctx.is_fk_cascading_ = true;
  if (OB_FAIL(op.begin_nested_session(false))) {
    LOG_WARN("failed to begin nested session", K(ret));
  } else {
    if (OB_FAIL(op.execute_write(sql))) {
      LOG_WARN("failed to maintain AI column tasks", K(ret), KCSTRING(sql));
    }
    int end_ret = op.end_nested_session();
    if (OB_SUCCESS != end_ret) {
      LOG_WARN("failed to end nested session", K(end_ret));
      ret = (OB_SUCCESS == ret) ? end_ret : ret;
    }
  }
  das_ctx.is_fk_cascading_ = save_fk_cascading;
  return ret;
}

int ObAIColumnDmlHandle::after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef, ObDmlEventType event)
{
  int ret = OB_SUCCESS;
  if (need_maintain(op, ctdef)) {
    const ObAIColumnDmlArg &arg = ctdef.ai_arg_;
    ObArenaAllocator alloc("AIColDml");
    ObSqlString del_sql;
    ObSqlString ins_sql;
    ObString old_pred;
    ObString new_pred;
    const bool is_insert = (ObDmlEventType::DE_INSERTING == event);
    const bool is_update = (ObDmlEventType::DE_UPDATING == event);
    const bool is_delete = (ObDmlEventType::DE_DELETING == event);
    if (!is_delete && OB_FAIL(gen_predicate(op, arg, ctdef.new_row_, alloc, new_pred))) {
    } else if (!is_insert && OB_FAIL(gen_predicate(op, arg, ctdef.old_row_, alloc, old_pred))) {
    }
    int64_t ins_cnt = 0;
    ObSqlString del_cols;
    const AIRowRequest *recorded = (OB_SUCC(ret) && !is_delete) ? find_row_request(op, new_pred) : nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && !is_delete && i < arg.cols_.count(); ++i) {
      const ObAIColumnDmlCol &col = arg.cols_.at(i);
      bool new_null = true;
      int64_t new_req = 0;
      bool old_null = true;
      int64_t old_req = 0;
      bool invalidated = false;
      if (OB_NOT_NULL(recorded)) {
        invalidated = i < recorded->invalidated_.length() && '1' == recorded->invalidated_.ptr()[i];
        new_req = recorded->request_id_;
      } else if (OB_FAIL(get_int(op, ctdef.new_row_, col.req_idx_, new_null, new_req))) {
      } else if (is_update && OB_FAIL(get_int(op, ctdef.old_row_, col.req_idx_, old_null, old_req))) {
      } else {
        invalidated = !new_null && !(is_update && !old_null && old_req == new_req);
      }
      if (OB_FAIL(ret)) {
      } else if (!invalidated) {
        // not invalidated by this statement
      } else {
        if (is_update && OB_FAIL(del_cols.append_fmt("%s%lu", del_cols.empty() ? "" : ",", col.column_id_))) {
        } else if (OB_FAIL(ins_sql.append_fmt("%s(%lu, %ld, %lu, ", 0 == ins_cnt ? "" : ", ", arg.table_id_,
                                              new_req, col.column_id_))) {
        } else if (OB_FAIL(ObAIColumnUtil::append_sql_string(ins_sql, new_pred))) {
        } else if (OB_FAIL(ins_sql.append_fmt(", %d, 0, now(6), %ld)", AI_TASK_STATE_PENDING, col.version_))) {
        } else {
          ins_cnt++;
        }
      }
    }
    // drop superseded / orphaned tasks of the old row
    if (OB_SUCC(ret) && (is_delete || !del_cols.empty())) {
      if (OB_FAIL(del_sql.assign_fmt("DELETE FROM %s.%s WHERE table_id = %lu AND rowkey_predicate = ",
                                     OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME, arg.table_id_))) {
      } else if (OB_FAIL(ObAIColumnUtil::append_sql_string(del_sql, old_pred))) {
      } else if (!is_delete && OB_FAIL(del_sql.append_fmt(" AND column_id IN (%.*s)",
                                                          static_cast<int>(del_cols.length()), del_cols.ptr()))) {
      } else if (OB_FAIL(execute_nested(op, del_sql.ptr()))) {
      }
    }
    if (OB_SUCC(ret) && ins_cnt > 0) {
      ObSqlString sql;
      if (OB_FAIL(sql.assign_fmt("INSERT INTO %s.%s (table_id, request_id, column_id, rowkey_predicate, state, "
                                 "retry_count, eligible_ts, definition_version) VALUES %.*s",
                                 OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME,
                                 static_cast<int>(ins_sql.length()), ins_sql.ptr()))) {
      } else if (OB_FAIL(execute_nested(op, sql.ptr()))) {
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
