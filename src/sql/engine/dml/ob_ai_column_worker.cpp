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

#include <cerrno>
#include <cfloat>
#include <cmath>
#include "sql/engine/dml/ob_ai_column_worker.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/time/ob_time_utility.h"
#include "share/ob_server_struct.h"
#include "share/object/ob_obj_cast.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace share::schema;
namespace sql
{

static const char *HIDDEN_HINT = "/*+ opt_param('hidden_column_visible', 'true') */";

ObAIColumnWorker &ObAIColumnWorker::get_instance()
{
  static ObAIColumnWorker instance;
  return instance;
}

int ObAIColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (FALSE_IT(set_run_wrapper(share::server_runtime()))) {
  } else if (OB_FAIL(lib::Threads::init())) {
    LOG_WARN("failed to init AI column worker", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAIColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(lib::Threads::start())) {
    LOG_WARN("failed to start AI column worker", K(ret));
  } else {
    LOG_INFO("AI column worker started", "threads", WORKER_THREAD_COUNT);
  }
  return ret;
}

void ObAIColumnWorker::stop()
{
  lib::Threads::stop();
}

void ObAIColumnWorker::wait()
{
  lib::Threads::wait();
}

bool ObAIColumnWorker::is_service_ready() const
{
  return OB_NOT_NULL(GCTX.sql_proxy_) && OB_NOT_NULL(GCTX.schema_service_) && !GCTX.in_bootstrap_
         && GCTX.start_service_time_ > 0;
}

void ObAIColumnWorker::run(int64_t idx)
{
  lib::set_thread_name("AIColumnWorker");
  while (!has_set_stop()) {
    int ret = OB_SUCCESS;
    bool claimed = false;
    uint64_t table_id = OB_INVALID_ID;
    int64_t request_id = 0;
    if (!is_service_ready()) {
      ob_usleep(1000 * 1000);
      continue;
    }
    if (0 == idx && !ATOMIC_LOAD(&recovered_)) {
      if (OB_FAIL(recover_running_tasks())) {
        LOG_WARN("failed to recover running AI column tasks", K(ret));
        ob_usleep(1000 * 1000);
        continue;
      }
      ATOMIC_STORE(&recovered_, true);
    }
    if (0 == idx && ATOMIC_LOAD(&recovered_)
        && ObTimeUtility::current_time() - last_sweep_ts_ > SWEEP_INTERVAL_US) {
      last_sweep_ts_ = ObTimeUtility::current_time();
      sweep_dropped_tables();
    }
    if (!ATOMIC_LOAD(&recovered_)) {
      ob_usleep(IDLE_SLEEP_US);
    } else if (OB_FAIL(claim_group(table_id, request_id, claimed))) {
      LOG_WARN("failed to claim AI column task", K(ret));
      ob_usleep(IDLE_SLEEP_US * 5);
    } else if (!claimed) {
      ob_usleep(IDLE_SLEEP_US);
    } else if (OB_FAIL(process_group(table_id, request_id))) {
      LOG_WARN("failed to process AI column task group", K(ret), K(table_id), K(request_id));
      // make the group claimable again after a short delay
      ObSqlString sql;
      int64_t affected = 0;
      if (OB_SUCCESS == sql.assign_fmt("UPDATE %s.%s SET state = %d, eligible_ts = date_add(now(6), interval 1 second) "
                                       "WHERE table_id = %lu AND request_id = %ld AND state = %d",
                                       OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME,
                                       AI_TASK_STATE_RETRY_WAIT, table_id, request_id, AI_TASK_STATE_RUNNING)) {
        (void)GCTX.sql_proxy_->write(sql.ptr(), affected);
      }
    }
  }
}

// AI COLUMN metadata and tasks of dropped tables
void ObAIColumnWorker::sweep_dropped_tables()
{
  const char *tables[2] = {share::OB_ALL_AI_COLUMN_TNAME, share::OB_ALL_AI_COLUMN_TASK_TNAME};
  for (int64_t i = 0; i < 2; ++i) {
    ObSqlString sql;
    int64_t affected = 0;
    if (OB_SUCCESS == sql.assign_fmt("DELETE FROM %s.%s WHERE table_id NOT IN (SELECT table_id FROM %s.%s)",
                                     OB_SYS_DATABASE_NAME, tables[i], OB_SYS_DATABASE_NAME,
                                     share::OB_ALL_TABLE_TNAME)) {
      (void)GCTX.sql_proxy_->write(sql.ptr(), affected);
    }
  }
}

int ObAIColumnWorker::recover_running_tasks()
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected = 0;
  // a crash or restart interrupted these requests: run them again
  if (OB_FAIL(sql.assign_fmt("UPDATE %s.%s SET state = %d WHERE state = %d", OB_SYS_DATABASE_NAME,
                             share::OB_ALL_AI_COLUMN_TASK_TNAME, AI_TASK_STATE_PENDING, AI_TASK_STATE_RUNNING))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
  } else if (affected > 0) {
    LOG_INFO("recovered interrupted AI column tasks", K(affected));
  }
  return ret;
}

int ObAIColumnWorker::claim_group(uint64_t &table_id, int64_t &request_id, bool &claimed)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSEArray<std::pair<uint64_t, int64_t>, 8> candidates;
  claimed = false;
  if (OB_FAIL(sql.assign_fmt("SELECT table_id, request_id FROM %s.%s WHERE state IN (%d, %d) "
                             "AND eligible_ts <= now(6) GROUP BY table_id, request_id "
                             "ORDER BY min(eligible_ts) LIMIT 16",
                             OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME,
                             AI_TASK_STATE_PENDING, AI_TASK_STATE_RETRY_WAIT))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret)) {
        int64_t tid = 0;
        int64_t rid = 0;
        if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          }
        } else if (OB_FAIL(result->get_int(0L, tid))) {
        } else if (OB_FAIL(result->get_int(1L, rid))) {
        } else {
          ret = candidates.push_back(std::make_pair(static_cast<uint64_t>(tid), rid));
        }
      }
    }
  }
  // several worker threads race for the same groups: the update decides
  for (int64_t i = 0; OB_SUCC(ret) && !claimed && i < candidates.count(); ++i) {
    int64_t affected = 0;
    if (OB_FAIL(sql.assign_fmt("UPDATE %s.%s SET state = %d WHERE table_id = %lu AND request_id = %ld "
                               "AND state IN (%d, %d) AND eligible_ts <= now(6)",
                               OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME, AI_TASK_STATE_RUNNING,
                               candidates.at(i).first, candidates.at(i).second,
                               AI_TASK_STATE_PENDING, AI_TASK_STATE_RETRY_WAIT))) {
    } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
    } else if (affected > 0) {
      claimed = true;
      table_id = candidates.at(i).first;
      request_id = candidates.at(i).second;
    }
  }
  return ret;
}

int ObAIColumnWorker::drop_group(uint64_t table_id, int64_t request_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s.%s WHERE table_id = %lu AND request_id = %ld",
                             OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME, table_id, request_id))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
  }
  return ret;
}

int ObAIColumnWorker::call_model(const ObAIColumnDef &def, const ObString &input, const ObString &extra_arg,
                                 ObIAllocator &allocator, ObString &raw_result, ObSqlString &error)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObJsonObject *config = nullptr;
  ObString model_name = def.model_;
  if (OB_ISNULL(resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
    LOG_WARN("AI model not found", K(ret), K(model_name));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint, false))) {
    LOG_WARN("AI endpoint not found", K(ret), K(model_name));
  } else if (AI_COLUMN_FUNC_COMPLETE == def.func_) {
    ObString config_str = extra_arg;
    if (!extra_arg.empty() && OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator, config_str, config))) {
      LOG_WARN("invalid AI_COMPLETE config", K(ret), K(extra_arg));
    } else {
      ObAIFuncModel model(allocator, *info, endpoint);
      ObString system_prompt = def.system_prompt_;
      ObString prompt = input;
      ret = model.call_completion_with_system(system_prompt, prompt, config, raw_result);
    }
  } else {
    ObJsonInt *dim_json = nullptr;
    if (!extra_arg.empty()) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, def.dimension_, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_SUCC(ret)) {
      ObAIFuncModel model(allocator, *info, endpoint);
      ObString content = input;
      ret = model.call_dense_embedding(content, config, raw_result);
    }
  }
  if (OB_FAIL(ret)) {
    (void)error.assign_fmt("model call failed: %d", ret);
  }
  return ret;
}

static ObString trim_ws(const ObString &s)
{
  const char *p = s.ptr();
  const char *e = s.ptr() + s.length();
  while (p < e && (' ' == *p || '\n' == *p || '\r' == *p || '\t' == *p)) {
    ++p;
  }
  while (e > p && (' ' == e[-1] || '\n' == e[-1] || '\r' == e[-1] || '\t' == e[-1])) {
    --e;
  }
  return ObString(static_cast<int32_t>(e - p), p);
}

// models often wrap structured answers in ```json ... ``` fences
static ObString strip_code_fence(const ObString &s)
{
  ObString t = trim_ws(s);
  if (t.length() >= 6 && 0 == MEMCMP(t.ptr(), "```", 3) && 0 == MEMCMP(t.ptr() + t.length() - 3, "```", 3)) {
    const char *p = t.ptr() + 3;
    const char *e = t.ptr() + t.length() - 3;
    while (p < e && '\n' != *p) {
      ++p;   // language tag
    }
    t = trim_ws(ObString(static_cast<int32_t>(e - p), p));
  }
  return t;
}

static int check_by_sql(const char *fmt, const ObString &literal, bool &ok)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ok = false;
  if (OB_FAIL(sql.assign_fmt(fmt, literal.length(), literal.ptr()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = nullptr;
      int64_t v = 0;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        ret = OB_SUCCESS;   // conversion error: not convertible
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_SUCC(result->next()) && OB_SUCC(result->get_int(0L, v))) {
        ok = (1 == v);
      } else {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

static bool is_decimal_literal(const ObString &value)
{
  int64_t i = 0;
  if (i < value.length() && ('+' == value.ptr()[i] || '-' == value.ptr()[i])) {
    ++i;
  }
  int64_t digits = 0;
  while (i < value.length() && value.ptr()[i] >= '0' && value.ptr()[i] <= '9') {
    ++i;
    ++digits;
  }
  if (i < value.length() && '.' == value.ptr()[i]) {
    ++i;
    while (i < value.length() && value.ptr()[i] >= '0' && value.ptr()[i] <= '9') {
      ++i;
      ++digits;
    }
  }
  bool valid = digits > 0;
  if (valid && i < value.length() && ('e' == value.ptr()[i] || 'E' == value.ptr()[i])) {
    ++i;
    if (i < value.length() && ('+' == value.ptr()[i] || '-' == value.ptr()[i])) {
      ++i;
    }
    const int64_t exponent_begin = i;
    while (i < value.length() && value.ptr()[i] >= '0' && value.ptr()[i] <= '9') {
      ++i;
    }
    valid = i > exponent_begin;
  }
  return valid && i == value.length();
}

int ObAIColumnWorker::to_literal(const ObColumnSchemaV2 &column, const ObAIColumnDef &def, const ObString &raw,
                                 ObIAllocator &allocator, ObString &literal, ObSqlString &error)
{
  int ret = OB_SUCCESS;
  ObSqlString lit;
  bool ok = true;
  const ObObjType type = column.get_data_type();
  ObString value = trim_ws(raw);
  if (AI_COLUMN_FUNC_EMBED == def.func_) {
    // "[v1,v2,...]" with the declared dimension
    int64_t dims = 0;
    bool in_num = false;
    for (int64_t i = 0; i < value.length(); ++i) {
      const char c = value.ptr()[i];
      if (',' == c) {
        in_num = false;
      } else if (!in_num && c != '[' && c != ']' && c != ' ') {
        in_num = true;
        dims++;
      }
    }
    ok = value.length() >= 2 && '[' == value.ptr()[0] && ']' == value.ptr()[value.length() - 1]
         && (def.dimension_ <= 0 || dims == def.dimension_);
    if (ok) {
      ret = ObAIColumnUtil::append_sql_string(lit, value);
    }
  } else if (ob_is_json(type)) {
    value = strip_code_fence(value);
    if (OB_FAIL(ObAIColumnUtil::append_sql_string(lit, value))) {
    } else if (OB_FAIL(check_by_sql("SELECT JSON_VALID(%.*s)", ObString(lit.length(), lit.ptr()), ok))) {
    }
  } else if (ObTinyIntType == type && 1 == column.get_accuracy().get_precision()) {
    if (0 == value.case_compare("true") || 0 == value.case_compare("1") || 0 == value.case_compare("yes")) {
      ret = lit.assign("1");
    } else if (0 == value.case_compare("false") || 0 == value.case_compare("0") || 0 == value.case_compare("no")) {
      ret = lit.assign("0");
    } else {
      ok = false;
    }
  } else if (ob_is_integer_type(type)) {
    int64_t i = 0;
    ok = value.length() > 0;
    const bool negative = ok && '-' == value.ptr()[0];
    const bool is_unsigned = ob_is_unsigned_type(type);
    const uint64_t limit = is_unsigned ? UINT_MAX_VAL[type]
        : negative ? static_cast<uint64_t>(-(INT_MIN_VAL[type] + 1)) + 1
                   : static_cast<uint64_t>(INT_MAX_VAL[type]);
    uint64_t magnitude = 0;
    if (ok && ('-' == value.ptr()[0] || '+' == value.ptr()[0])) {
      i = 1;
      ok = value.length() > 1;
    }
    for (; ok && i < value.length(); ++i) {
      ok = (value.ptr()[i] >= '0' && value.ptr()[i] <= '9');
      if (ok) {
        const uint64_t digit = static_cast<uint64_t>(value.ptr()[i] - '0');
        ok = magnitude <= limit / 10 && (magnitude != limit / 10 || digit <= limit % 10);
        if (ok) {
          magnitude = magnitude * 10 + digit;
        }
      }
    }
    ok = ok && !(is_unsigned && negative && magnitude != 0);
    // Validate the actual destination width before publishing. CAST AS SIGNED
    // accepts values which cannot be stored in TINYINT/SMALLINT/etc.
    if (ok) {
      ret = lit.assign(value.ptr(), value.length());
    }
  } else if (ob_is_float_tc(type) || ob_is_double_tc(type) || ob_is_number_tc(type) || ob_is_decimal_int(type)) {
    char buf[128];
    char *end = nullptr;
    ok = is_decimal_literal(value) && value.length() < static_cast<int64_t>(sizeof(buf));
    if (ok) {
      MEMCPY(buf, value.ptr(), value.length());
      buf[value.length()] = '\0';
      errno = 0;
      const double number = strtod(buf, &end);
      ok = end == buf + value.length() && ERANGE != errno && std::isfinite(number)
           && !(ob_is_unsigned_type(type) && number < 0)
           && !(ob_is_float_type(type) && std::fabs(number) > FLT_MAX);
    }
    if (ok && (ob_is_number_tc(type) || ob_is_decimal_int(type))) {
      // A finite double can still exceed DECIMAL(p,s). Use the engine's exact
      // decimal range/rounding rules before attempting the publication UPDATE.
      number::ObNumber number;
      ObObj source;
      ObObj converted;
      const ObObj *checked = nullptr;
      ObCastCtx cast_ctx(&allocator, nullptr, CM_NONE, column.get_collation_type());
      if (OB_SUCCESS != number.from(value.ptr(), value.length(), allocator)) {
        ok = false;
      } else {
        source.set_number(number);
        ok = OB_SUCCESS == obj_accuracy_check(cast_ctx, column.get_accuracy(),
                                              column.get_collation_type(), source, converted, checked);
      }
    }
    if (ok) {
      ret = lit.assign(value.ptr(), value.length());
    }
  } else if (ob_is_date_tc(type) || ObMySQLDateType == type) {
    if (OB_FAIL(ObAIColumnUtil::append_sql_string(lit, value))) {
    } else if (OB_FAIL(check_by_sql("SELECT CAST(%.*s AS DATE) IS NOT NULL", ObString(lit.length(), lit.ptr()), ok))) {
    }
  } else if (ob_is_datetime_tc(type) || ObMySQLDateTimeType == type || ObTimestampType == type) {
    if (OB_FAIL(ObAIColumnUtil::append_sql_string(lit, value))) {
    } else if (OB_FAIL(check_by_sql("SELECT CAST(%.*s AS DATETIME(6)) IS NOT NULL", ObString(lit.length(), lit.ptr()), ok))) {
    }
  } else {
    // character types keep the answer as is, it must fit the column
    if (ob_is_string_tc(type) && column.get_data_length() > 0
        && ObCharset::strlen_char(column.get_collation_type(), raw.ptr(), raw.length()) > column.get_data_length()) {
      ok = false;
    } else {
      ret = ObAIColumnUtil::append_sql_string(lit, raw);
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!ok) {
    (void)error.assign_fmt("model answer does not match column type %s", ob_obj_type_str(type));
    ret = OB_INVALID_DATA;
  } else {
    ret = ob_write_string(allocator, ObString(lit.length(), lit.ptr()), literal);
  }
  return ret;
}

int ObAIColumnWorker::schedule_retry(uint64_t table_id, int64_t request_id, ObIArray<TaskCol> &cols)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  int64_t max_retry = 0;
  for (int64_t i = 0; i < cols.count(); ++i) {
    max_retry = MAX(max_retry, cols.at(i).retry_count_);
  }
  // exponential backoff: 200ms, 400ms, 800ms ... capped
  int64_t delay = BASE_RETRY_DELAY_US;
  for (int64_t i = 0; i < max_retry && delay < MAX_RETRY_DELAY_US; ++i) {
    delay *= 2;
  }
  delay = MIN(delay, MAX_RETRY_DELAY_US);
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    TaskCol &col = cols.at(i);
    ObSqlString sql;
    int64_t affected = 0;
    if (!col.live_) {
      ret = sql.assign_fmt("DELETE FROM %s.%s WHERE table_id = %lu AND request_id = %ld AND column_id = %lu",
                           OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME, table_id, request_id,
                           col.column_id_);
    } else {
      if (OB_FAIL(sql.assign_fmt("UPDATE %s.%s SET state = %d, retry_count = %ld, "
                                 "eligible_ts = date_add(now(6), interval %ld microsecond), result_state = %d, "
                                 "result_value = ",
                                 OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME, AI_TASK_STATE_RETRY_WAIT,
                                 col.retry_count_ + (!col.ok_ && col.attempted_ ? 1 : 0),
                                 delay, col.ok_ ? 1 : 0))) {
      } else if (col.ok_ && OB_FAIL(ObAIColumnUtil::append_sql_string(sql, col.literal_))) {
      } else if (!col.ok_ && OB_FAIL(sql.append("NULL"))) {
      } else if (OB_FAIL(sql.append(", last_error = "))) {
      } else if (OB_FAIL(ObAIColumnUtil::append_sql_string(sql, col.error_))) {
      } else if (OB_FAIL(sql.append_fmt(" WHERE table_id = %lu AND request_id = %ld AND column_id = %lu",
                                        table_id, request_id, col.column_id_))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(trans.write(sql.ptr(), affected))) {
    }
  }
  if (trans.is_started()) {
    int tmp_ret = trans.end(OB_SUCCESS == ret);
    ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
  }
  return ret;
}

int ObAIColumnWorker::publish(uint64_t table_id, int64_t request_id, const ObString &full_table_name,
                              const ObString &predicate, ObIArray<TaskCol> &cols)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
  }
  // all results and states of the request become visible at once
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    TaskCol &col = cols.at(i);
    ObSqlString sql;
    int64_t affected = 0;
    if (!col.live_ || OB_ISNULL(col.def_)) {
      continue;
    }
    const ObAIColumnDef &def = *col.def_;
    if (OB_FAIL(sql.assign_fmt("UPDATE %s %.*s SET ", HIDDEN_HINT, full_table_name.length(), full_table_name.ptr()))) {
    } else if (OB_FAIL(ObAIColumnUtil::append_ident(sql, def.column_name_))) {
    } else if (OB_FAIL(sql.append(" = "))) {
    } else if (col.ok_ && OB_FAIL(sql.append(col.literal_))) {
    } else if (!col.ok_ && OB_FAIL(sql.append("NULL"))) {
    } else if (OB_FAIL(sql.append_fmt(", `%s%.*s` = %d WHERE (%.*s) AND `%s%.*s` = %ld",
                                      ObAIColumnUtil::STATE_PREFIX, def.column_name_.length(), def.column_name_.ptr(),
                                      col.ok_ ? AI_COLUMN_STATE_READY : AI_COLUMN_STATE_FAILED,
                                      predicate.length(), predicate.ptr(),
                                      ObAIColumnUtil::REQUEST_PREFIX, def.column_name_.length(), def.column_name_.ptr(),
                                      request_id))) {
    } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
      LOG_WARN("failed to publish AI column result", K(ret), K(sql));
    } else if (0 == affected) {
      // superseded by a newer request or the row was deleted: nothing to publish
      LOG_TRACE("stale AI column result dropped", K(table_id), K(request_id), K(def));
    }
  }
  if (OB_SUCC(ret)) {
    ObSqlString sql;
    int64_t affected = 0;
    if (OB_FAIL(sql.assign_fmt("DELETE FROM %s.%s WHERE table_id = %lu AND request_id = %ld",
                               OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME, table_id, request_id))) {
    } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
    }
  }
  if (trans.is_started()) {
    int tmp_ret = trans.end(OB_SUCCESS == ret);
    ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
  }
  return ret;
}

int ObAIColumnWorker::process_group(uint64_t table_id, int64_t request_id)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColWorker");
  ObSEArray<TaskCol, 4> cols;
  ObString predicate;
  ObSqlString sql;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table_schema = nullptr;
  const ObDatabaseSchema *db_schema = nullptr;
  ObSEArray<ObAIColumnDef, 4> defs;
  ObSqlString full_name;
  // 1. task rows of the group
  if (OB_FAIL(sql.assign_fmt("SELECT column_id, rowkey_predicate, retry_count, definition_version, result_state, "
                             "result_value FROM %s.%s WHERE table_id = %lu AND request_id = %ld",
                             OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME, table_id, request_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret)) {
        TaskCol col;
        int64_t column_id = 0;
        int64_t result_state = 0;
        ObString pred;
        ObString cached;
        if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          }
        } else if (OB_FAIL(result->get_int(0L, column_id))) {
        } else if (OB_FAIL(result->get_varchar(1L, pred))) {
        } else if (OB_FAIL(result->get_int(2L, col.retry_count_))) {
        } else if (OB_FAIL(result->get_int(3L, col.version_))) {
        } else if (OB_FAIL(result->get_int(4L, result_state))) {
        } else {
          if (1 == result_state) {
            int tmp_ret = result->get_varchar(5L, cached);
            if (OB_SUCCESS == tmp_ret && OB_SUCC(ob_write_string(allocator, cached, col.cached_value_))) {
              col.cached_ = true;
            }
          }
          col.column_id_ = static_cast<uint64_t>(column_id);
          if (OB_FAIL(ret)) {
          } else if (predicate.empty() && OB_FAIL(ob_write_string(allocator, pred, predicate))) {
          } else {
            ret = cols.push_back(col);
          }
        }
      }
    }
  }
  // 2. table definition
  if (OB_FAIL(ret) || cols.empty()) {
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    // the table was dropped
    ret = drop_group(table_id, request_id);
    cols.reset();
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIColumnUtil::get_ai_columns(*table_schema, allocator, defs))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_ident(full_name, db_schema->get_database_name_str()))) {
  } else if (OB_FAIL(full_name.append("."))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_ident(full_name, table_schema->get_table_name_str()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    TaskCol &col = cols.at(i);
    for (int64_t j = 0; nullptr == col.def_ && j < defs.count(); ++j) {
      if (defs.at(j).column_id_ == col.column_id_) {
        col.def_ = &defs.at(j);
      }
    }
  }
  // 3. read the inputs of the row and check which requests are still current
  ObSEArray<ObString, 4> inputs;
  ObSEArray<ObString, 4> extras;
  ObSEArray<bool, 4> input_null;
  bool row_exists = false;
  if (OB_SUCC(ret) && !cols.empty()) {
    sql.reuse();
    if (OB_FAIL(sql.assign_fmt("SELECT %s ", HIDDEN_HINT))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
      const ObAIColumnDef *def = cols.at(i).def_;
      if (OB_ISNULL(def)) {
        ret = sql.append_fmt("%sNULL, NULL, NULL", 0 == i ? "" : ", ");
      } else if (OB_FAIL(sql.append_fmt("%sCAST((%.*s) AS CHAR), ", 0 == i ? "" : ", ",
                                        def->input_expr_.length(), def->input_expr_.ptr()))) {
      } else if (def->extra_arg_.empty() && OB_FAIL(sql.append("NULL, "))) {
      } else if (!def->extra_arg_.empty() && OB_FAIL(sql.append_fmt("CAST((%.*s) AS CHAR), ",
                                                                     def->extra_arg_.length(), def->extra_arg_.ptr()))) {
      } else if (OB_FAIL(sql.append_fmt("`%s%.*s`", ObAIColumnUtil::REQUEST_PREFIX,
                                        def->column_name_.length(), def->column_name_.ptr()))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(" FROM %.*s WHERE %.*s", static_cast<int>(full_name.length()),
                                               full_name.ptr(), predicate.length(), predicate.ptr()))) {
    }
    if (OB_SUCC(ret)) {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        common::sqlclient::ObMySQLResult *result = nullptr;
        if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
          LOG_WARN("failed to read AI column inputs", K(ret), K(sql));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;   // the row is gone
          }
        } else {
          row_exists = true;
          for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
            TaskCol &col = cols.at(i);
            ObString input;
            ObString extra;
            ObString copied_input;
            ObString copied_extra;
            int64_t req = 0;
            bool is_null = false;
            int tmp_ret = result->get_varchar(i * 3, input);
            is_null = (OB_ERR_NULL_VALUE == tmp_ret);
            if (OB_SUCCESS != tmp_ret && !is_null) {
              ret = tmp_ret;
            } else if (OB_SUCCESS != (tmp_ret = result->get_varchar(i * 3 + 1, extra))
                       && OB_ERR_NULL_VALUE != tmp_ret) {
              ret = tmp_ret;
            } else if (OB_SUCCESS == (tmp_ret = result->get_int(i * 3 + 2, req))) {
              col.live_ = OB_NOT_NULL(col.def_) && req == request_id
                          && col.version_ == col.def_->version_;
            }
            if (OB_FAIL(ret)) {
            } else if (OB_FAIL(ob_write_string(allocator, input, copied_input))) {
            } else if (OB_FAIL(ob_write_string(allocator, extra, copied_extra))) {
            } else if (OB_FAIL(inputs.push_back(copied_input))) {
            } else if (OB_FAIL(extras.push_back(copied_extra))) {
            } else if (OB_FAIL(input_null.push_back(is_null))) {
            }
          }
        }
      }
    }
  }
  if (OB_FAIL(ret) || cols.empty()) {
  } else if (!row_exists) {
    // deleted rows are never restored by late results
    ret = drop_group(table_id, request_id);
  } else {
    // 4. call the models of the live columns
    bool all_terminal = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
      TaskCol &col = cols.at(i);
      if (!col.live_) {
        col.terminal_ = true;
        continue;
      }
      const ObColumnSchemaV2 *column = table_schema->get_column_schema(col.column_id_);
      ObString raw;
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (col.cached_) {
        col.ok_ = true;
        col.literal_ = col.cached_value_;
      } else if (col.retry_count_ > col.def_->max_retries_) {
        // A sibling can still be retrying after this column has exhausted its
        // own budget. Keep its failure without issuing another model request.
        col.ok_ = false;
      } else if (input_null.at(i)) {
        // nothing to ask: the result stays NULL and the column is READY
        col.ok_ = true;
        col.literal_ = ObString("NULL");
      } else {
        ObSqlString err;
        col.attempted_ = true;
        int call_ret = call_model(*col.def_, inputs.at(i), extras.at(i), allocator, raw, err);
        if (OB_SUCCESS == call_ret) {
          call_ret = to_literal(*column, *col.def_, raw, allocator, col.literal_, err);
        }
        if (OB_SUCCESS != call_ret) {
          (void)ob_write_string(allocator, ObString(err.length(), err.ptr()), col.error_);
        }
        col.ok_ = (OB_SUCCESS == call_ret);
        if (!col.ok_) {
          LOG_INFO("AI column request failed", K(call_ret), K(table_id), K(request_id), K(col));
        }
      }
      if (OB_SUCC(ret)) {
        col.terminal_ = col.ok_ || col.retry_count_ >= col.def_->max_retries_;
        all_terminal = all_terminal && col.terminal_;
      }
    }
    // 5. publish the group, or keep successes and retry the failures
    if (OB_FAIL(ret)) {
    } else if (all_terminal) {
      ret = publish(table_id, request_id, ObString(full_name.length(), full_name.ptr()), predicate, cols);
    } else {
      ret = schedule_retry(table_id, request_id, cols);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
