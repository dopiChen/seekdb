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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/ddl/ob_ai_column_util.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "lib/time/ob_time_utility.h"
#include "query/api/query/vector/ob_vector_index_util.h"
#include "share/ob_errno.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{

////////////////////////////////////////////////////////////////////////////////
// ObAIColumnDef

void ObAIColumnDef::reset()
{
  column_name_.reset();
  func_ = AI_COLUMN_FUNC_INVALID;
  model_.reset();
  input_expr_.reset();
  extra_arg_.reset();
  deps_.reset();
  max_retries_ = ObAIColumnUtil::DEFAULT_MAX_RETRIES;
  sync_mode_ = ObString("async");
  result_type_.reset();
  dimension_ = 0;
  version_ = 1;
  system_prompt_.reset();
  column_id_ = OB_INVALID_ID;
  state_column_id_ = OB_INVALID_ID;
  request_column_id_ = OB_INVALID_ID;
  spec_column_id_ = OB_INVALID_ID;
}

// "<tag>:<len>:<bytes>" items, strings are length prefixed
static int append_item(ObSqlString &out, char tag, const ObString &val)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(out.append_fmt("%c:%d:", tag, val.length()))) {
  } else if (val.length() > 0) {
    ret = out.append(val.ptr(), val.length());
  }
  return ret;
}

static int append_int_item(ObSqlString &out, char tag, int64_t v)
{
  char buf[32];
  int64_t len = snprintf(buf, sizeof(buf), "%ld", v);
  return append_item(out, tag, ObString(static_cast<int32_t>(len), buf));
}

int ObAIColumnDef::encode(ObSqlString &out) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_item(out, 'N', column_name_))) {
  } else if (OB_FAIL(append_int_item(out, 'F', func_))) {
  } else if (OB_FAIL(append_item(out, 'M', model_))) {
  } else if (OB_FAIL(append_item(out, 'I', input_expr_))) {
  } else if (OB_FAIL(append_item(out, 'X', extra_arg_))) {
  } else if (OB_FAIL(append_int_item(out, 'R', max_retries_))) {
  } else if (OB_FAIL(append_item(out, 'S', sync_mode_))) {
  } else if (OB_FAIL(append_item(out, 'T', result_type_))) {
  } else if (OB_FAIL(append_int_item(out, 'D', dimension_))) {
  } else if (OB_FAIL(append_int_item(out, 'V', version_))) {
  } else if (OB_FAIL(append_item(out, 'P', system_prompt_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < deps_.count(); ++i) {
    ret = append_item(out, 'C', deps_.at(i));
  }
  return ret;
}

int ObAIColumnDef::decode(const ObString &in, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  reset();
  const char *p = in.ptr();
  const char *end = in.ptr() + in.length();
  while (OB_SUCC(ret) && p < end) {
    const char tag = *p++;
    int64_t len = 0;
    if (p >= end || ':' != *p++) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      while (p < end && *p >= '0' && *p <= '9') {
        len = len * 10 + (*p++ - '0');
      }
      if (p >= end || ':' != *p++ || len > end - p) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    if (OB_SUCC(ret)) {
      ObString raw(static_cast<int32_t>(len), p);
      ObString val;
      int64_t iv = 0;
      bool neg = false;
      for (int64_t i = 0; i < len; ++i) {
        if (0 == i && '-' == p[i]) {
          neg = true;
        } else if (p[i] >= '0' && p[i] <= '9') {
          iv = iv * 10 + (p[i] - '0');
        }
      }
      iv = neg ? -iv : iv;
      if (OB_FAIL(ob_write_string(allocator, raw, val))) {
      } else {
        switch (tag) {
          case 'N': column_name_ = val; break;
          case 'F': func_ = static_cast<int32_t>(iv); break;
          case 'M': model_ = val; break;
          case 'I': input_expr_ = val; break;
          case 'X': extra_arg_ = val; break;
          case 'R': max_retries_ = iv; break;
          case 'S': sync_mode_ = val; break;
          case 'T': result_type_ = val; break;
          case 'D': dimension_ = iv; break;
          case 'V': version_ = iv; break;
          case 'P': system_prompt_ = val; break;
          case 'C': ret = deps_.push_back(val); break;
          default: break;   // ignore unknown tags of newer versions
        }
      }
      p += len;
    }
  }
  if (OB_SUCC(ret) && (column_name_.empty() || AI_COLUMN_FUNC_INVALID == func_)) {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

int ObAIColumnDef::build_normalized_expr(ObSqlString &out) const
{
  int ret = OB_SUCCESS;
  out.reuse();
  const char *func = (AI_COLUMN_FUNC_EMBED == func_) ? "AI_EMBED" : "AI_COMPLETE";
  if (OB_FAIL(out.append_fmt("%s(", func))) {
  } else if (OB_FAIL(ObAIColumnUtil::append_sql_string(out, model_))) {
  } else if (OB_FAIL(out.append_fmt(", %.*s", input_expr_.length(), input_expr_.ptr()))) {
  } else if (!extra_arg_.empty() && OB_FAIL(out.append_fmt(", %.*s", extra_arg_.length(), extra_arg_.ptr()))) {
  } else if (OB_FAIL(out.append(")"))) {
  } else if (!system_prompt_.empty()) {
    if (OB_FAIL(out.append(" WITH SYSTEM PROMPT "))) {
    } else {
      ret = ObAIColumnUtil::append_sql_string(out, system_prompt_);
    }
  }
  return ret;
}

bool ObAIColumnDef::depends_on(const ObString &column_name) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < deps_.count(); ++i) {
    found = (0 == deps_.at(i).case_compare(column_name));
  }
  return found;
}

////////////////////////////////////////////////////////////////////////////////
// spec resolution

// split "FUNC(a, b, c)" into argument texts, respecting quotes and parentheses
static int split_call_args(const ObString &text, ObString &func_name, ObIArray<ObString> &args)
{
  int ret = OB_SUCCESS;
  const char *p = text.ptr();
  const char *end = text.ptr() + text.length();
  while (p < end && (' ' == *p || '\t' == *p || '\n' == *p || '\r' == *p)) {
    ++p;
  }
  const char *name_start = p;
  while (p < end && '(' != *p) {
    ++p;
  }
  if (p >= end) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *name_end = p;
    while (name_end > name_start && (' ' == name_end[-1] || '\t' == name_end[-1])) {
      --name_end;
    }
    func_name.assign_ptr(name_start, static_cast<int32_t>(name_end - name_start));
    ++p;   // skip '('
    int depth = 0;
    const char *arg_start = p;
    bool closed = false;
    while (OB_SUCC(ret) && p < end && !closed) {
      const char c = *p;
      if ('\'' == c || '"' == c || '`' == c) {
        const char q = c;
        ++p;
        while (p < end && *p != q) {
          if ('\\' == *p && '`' != q) {
            ++p;
          }
          ++p;
        }
        ++p;
        continue;
      } else if ('(' == c) {
        depth++;
      } else if (')' == c) {
        if (0 == depth) {
          closed = true;
        } else {
          depth--;
        }
      }
      if ((',' == c && 0 == depth) || closed) {
        ObString arg(static_cast<int32_t>(p - arg_start), arg_start);
        arg = arg.trim();
        if (OB_FAIL(args.push_back(arg))) {
        }
        arg_start = p + 1;
      }
      ++p;
    }
    if (OB_SUCC(ret)) {
      while (p < end && (' ' == *p || '\t' == *p || '\n' == *p || '\r' == *p)) {
        ++p;
      }
      if (!closed || p != end) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
  }
  return ret;
}

static int collect_column_refs(const ParseNode *node, ObIAllocator &allocator, ObIArray<ObString> &deps)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(node)) {
    if (T_COLUMN_REF == node->type_ && 3 == node->num_child_ && OB_NOT_NULL(node->children_[2])) {
      ObString name(static_cast<int32_t>(node->children_[2]->str_len_), node->children_[2]->str_value_);
      bool exists = false;
      for (int64_t i = 0; !exists && i < deps.count(); ++i) {
        exists = (0 == deps.at(i).case_compare(name));
      }
      ObString copied;
      if (!exists && OB_SUCC(ob_write_string(allocator, name, copied))) {
        ret = deps.push_back(copied);
      }
    } else if (T_SELECT == node->type_) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "subquery in AI COLUMN expression");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      ret = collect_column_refs(node->children_[i], allocator, deps);
    }
  }
  return ret;
}

int ObAIColumnUtil::get_type_hint(const ObColumnSchemaV2 &column, ObString &hint)
{
  int ret = OB_SUCCESS;
  const ObObjType type = column.get_data_type();
  if (ob_is_json(type)) {
    hint = ObString("Return exactly one valid JSON value and nothing else.");
  } else if (ObTinyIntType == type && 1 == column.get_accuracy().get_precision()) {
    hint = ObString("Return exactly one of true or false and nothing else.");
  } else if (ob_is_integer_type(type)) {
    hint = ObString("Return exactly one base-10 integer and nothing else.");
  } else if (ob_is_float_tc(type) || ob_is_double_tc(type) || ob_is_number_tc(type)
             || ob_is_decimal_int(type)) {
    hint = ObString("Return exactly one decimal number and nothing else.");
  } else if (ob_is_date_tc(type) || ObMySQLDateType == type) {
    hint = ObString("Return exactly one date in YYYY-MM-DD format and nothing else.");
  } else if (ob_is_datetime_tc(type) || ObMySQLDateTimeType == type || ObTimestampType == type) {
    hint = ObString("Return exactly one datetime in YYYY-MM-DD HH:MM:SS format and nothing else.");
  } else {
    hint.reset();   // free text
  }
  return ret;
}

int ObAIColumnUtil::resolve_spec(const ParseNode &spec_node, const ObColumnSchemaV2 &column,
                                 ObIAllocator &allocator, ObAIColumnDef &def)
{
  int ret = OB_SUCCESS;
  def.reset();
  const ParseNode *expr = nullptr;
  const ParseNode *options = nullptr;
  ObString func_name;
  ObSEArray<ObString, 4> arg_texts;
  if (T_AI_COLUMN_SPEC != spec_node.type_ || 2 != spec_node.num_child_
      || OB_ISNULL(expr = spec_node.children_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ob_write_string(allocator, column.get_column_name_str(), def.column_name_))) {
  } else if (T_FUN_SYS != expr->type_ || 2 != expr->num_child_ || OB_ISNULL(expr->children_[0])
             || OB_ISNULL(expr->children_[1])) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression other than AI_COMPLETE or AI_EMBED");
  } else {
    options = spec_node.children_[1];
    ObString name(static_cast<int32_t>(expr->children_[0]->str_len_), expr->children_[0]->str_value_);
    const ParseNode *params = expr->children_[1];
    ObString text(static_cast<int32_t>(expr->str_len_), expr->str_value_);
    if (0 == name.case_compare("ai_complete")) {
      def.func_ = AI_COLUMN_FUNC_COMPLETE;
    } else if (0 == name.case_compare("ai_embed")) {
      def.func_ = AI_COLUMN_FUNC_EMBED;
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression other than AI_COMPLETE or AI_EMBED");
    }
    if (OB_FAIL(ret)) {
    } else if (params->num_child_ < 2 || params->num_child_ > 3) {
      ret = OB_INVALID_ARGUMENT_NUM;
    } else if (OB_ISNULL(params->children_[0]) || (T_VARCHAR != params->children_[0]->type_
                                                   && T_CHAR != params->children_[0]->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN model name that is not a string literal");
    } else if (OB_FAIL(ob_write_string(allocator, ObString(static_cast<int32_t>(params->children_[0]->str_len_),
                                                           params->children_[0]->str_value_), def.model_))) {
    } else if (OB_FAIL(split_call_args(text, func_name, arg_texts)) || arg_texts.count() != params->num_child_) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression form");
    } else if (OB_FAIL(ob_write_string(allocator, arg_texts.at(1), def.input_expr_))) {
    } else if (3 == params->num_child_ && OB_FAIL(ob_write_string(allocator, arg_texts.at(2), def.extra_arg_))) {
    } else if (OB_FAIL(collect_column_refs(params->children_[1], allocator, def.deps_))) {
    } else if (def.deps_.empty()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN without referenced columns");
    }
    // result type
    if (OB_SUCC(ret)) {
      const bool is_vector = ob_is_collection_sql_type(column.get_data_type());
      if (AI_COLUMN_FUNC_EMBED == def.func_) {
        int64_t dim = 0;
        if (!is_vector) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED AI COLUMN whose type is not VECTOR");
        } else if (OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                       column.get_extended_type_info(), dim))) {
          LOG_WARN("failed to get vector dim", K(ret));
        } else {
          def.dimension_ = dim;
          if (3 == params->num_child_) {
            const ParseNode *dim_node = params->children_[2];
            if (OB_ISNULL(dim_node) || T_INT != dim_node->type_) {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED dimension that is not an integer literal");
            } else if (dim_node->value_ != dim) {
              ret = OB_ERR_INVALID_VECTOR_DIM;
              LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(dim),
                             static_cast<uint32_t>(dim_node->value_));
            }
          }
        }
      } else if (is_vector) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_COMPLETE AI COLUMN of VECTOR type");
      } else if (ob_is_geometry(column.get_data_type())
                 || ob_is_enum_or_set_type(column.get_data_type())) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN of this type");
      }
    }
    if (OB_SUCC(ret)) {
      ObString hint;
      const char *type_str = ob_obj_type_str(column.get_data_type());
      if (OB_FAIL(get_type_hint(column, hint))) {
      } else if (AI_COLUMN_FUNC_COMPLETE == def.func_
                 && OB_FAIL(ob_write_string(allocator, hint, def.system_prompt_))) {
      } else if (OB_FAIL(ob_write_string(allocator, ObString(type_str), def.result_type_))) {
      }
    }
  }
  // options
  for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(options) && i < options->num_child_; ++i) {
    const ParseNode *opt = options->children_[i];
    if (OB_ISNULL(opt) || T_AI_COLUMN_OPTION != opt->type_ || 2 != opt->num_child_
        || OB_ISNULL(opt->children_[0]) || OB_ISNULL(opt->children_[1])) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      ObString key(static_cast<int32_t>(opt->children_[0]->str_len_), opt->children_[0]->str_value_);
      const ParseNode *val = opt->children_[1];
      ObString sval(static_cast<int32_t>(val->str_len_), val->str_value_);
      if (0 == key.case_compare("sync_mode")) {
        if (0 != sval.case_compare("async")) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN sync_mode other than async");
        } else {
          def.sync_mode_ = ObString("async");
        }
      } else if (0 == key.case_compare("max_retries")) {
        if (T_INT != val->type_ || val->value_ < 0 || val->value_ > MAX_MAX_RETRIES) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN max_retries");
        } else {
          def.max_retries_ = val->value_;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "this AI COLUMN option");
      }
    }
  }
  return ret;
}

int ObAIColumnUtil::check_dependencies(const ObTableSchema &table_schema,
                                       const ObIArray<ObAIColumnDef> &new_defs)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColDeps");
  ObSEArray<ObAIColumnDef, 4> old_defs;
  if (OB_FAIL(get_ai_columns(table_schema, alloc, old_defs))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < new_defs.count(); ++i) {
    const ObAIColumnDef &def = new_defs.at(i);
    for (int64_t j = 0; OB_SUCC(ret) && j < def.deps_.count(); ++j) {
      const ObString &dep = def.deps_.at(j);
      bool is_ai = false;
      for (int64_t k = 0; !is_ai && k < new_defs.count(); ++k) {
        is_ai = (0 == new_defs.at(k).column_name_.case_compare(dep));
      }
      for (int64_t k = 0; !is_ai && k < old_defs.count(); ++k) {
        is_ai = (0 == old_defs.at(k).column_name_.case_compare(dep));
      }
      const ObColumnSchemaV2 *dep_col = table_schema.get_column_schema(dep);
      if (is_ai || is_ai_hidden_column_name(dep)) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("AI column depends on another AI column", K(ret), K(def), K(dep));
      } else if (OB_ISNULL(dep_col)) {
        ret = OB_ERR_BAD_FIELD_ERROR;
        LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, dep.length(), dep.ptr(),
                       table_schema.get_table_name_str().length(), table_schema.get_table_name_str().ptr());
      } else if (dep_col->is_generated_column()) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN depending on a generated column");
      }
    }
  }
  return ret;
}

bool ObAIColumnUtil::is_ai_hidden_column_name(const ObString &name)
{
  return name.prefix_match(STATE_PREFIX) || name.prefix_match(REQUEST_PREFIX)
         || name.prefix_match(SPEC_PREFIX);
}

int ObAIColumnUtil::build_hidden_column(const ObAIColumnDef &def, const char *prefix, uint64_t table_id,
                                        uint64_t column_id, ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  ObSqlString name;
  ObObj default_value;
  column.reset();
  column.set_table_id(table_id);
  column.set_column_id(column_id);
  column.set_rowkey_position(0);
  column.set_index_position(0);
  column.set_tbl_part_key_pos(0);
  column.set_is_hidden(true);
  column.set_collation_type(CS_TYPE_BINARY);
  column.set_charset_type(CHARSET_BINARY);
  column.set_prev_column_id(UINT64_MAX);
  column.set_next_column_id(UINT64_MAX);
  if (0 == STRCMP(prefix, STATE_PREFIX)) {
    column.set_data_type(ObIntType);
    column.set_nullable(false);
    default_value.set_int(AI_COLUMN_STATE_PENDING);
  } else if (0 == STRCMP(prefix, REQUEST_PREFIX)) {
    column.set_data_type(ObIntType);
    column.set_nullable(true);
    default_value.set_null();
  } else {
    ObSqlString encoded;
    column.set_data_type(ObIntType);
    column.set_nullable(false);
    default_value.set_int(def.version_);
    if (OB_FAIL(def.encode(encoded))) {
    } else if (OB_FAIL(column.set_comment(ObString(encoded.length(), encoded.ptr())))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(name.append_fmt("%s%.*s", prefix, def.column_name_.length(), def.column_name_.ptr()))) {
  } else if (name.length() > OB_MAX_COLUMN_NAME_LENGTH) {
    ret = OB_WRONG_COLUMN_NAME;
    LOG_USER_ERROR(OB_WRONG_COLUMN_NAME, static_cast<int>(name.length()), name.ptr());
  } else if (OB_FAIL(column.set_column_name(ObString(name.length(), name.ptr())))) {
  } else if (OB_FAIL(column.set_orig_default_value(default_value))) {
  } else if (OB_FAIL(column.set_cur_default_value(default_value, false))) {
  }
  return ret;
}

int ObAIColumnUtil::add_hidden_columns(ObTableSchema &table_schema, ObAIColumnDef &def,
                                       uint64_t state_column_id, uint64_t request_column_id,
                                       uint64_t spec_column_id)
{
  int ret = OB_SUCCESS;
  const char *prefixes[3] = {STATE_PREFIX, REQUEST_PREFIX, SPEC_PREFIX};
  const uint64_t ids[3] = {state_column_id, request_column_id, spec_column_id};
  for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
    ObColumnSchemaV2 column;
    if (OB_FAIL(build_hidden_column(def, prefixes[i], table_schema.get_table_id(), ids[i], column))) {
    } else if (OB_FAIL(table_schema.add_column(column))) {
      LOG_WARN("failed to add AI hidden column", K(ret), K(column));
    }
  }
  if (OB_SUCC(ret)) {
    def.state_column_id_ = state_column_id;
    def.request_column_id_ = request_column_id;
    def.spec_column_id_ = spec_column_id;
  }
  return ret;
}

int ObAIColumnUtil::get_ai_columns(const ObTableSchema &table_schema, ObIAllocator &allocator,
                                   ObIArray<ObAIColumnDef> &defs)
{
  int ret = OB_SUCCESS;
  defs.reset();
  ObTableSchema::const_column_iterator it = table_schema.column_begin();
  for (; OB_SUCC(ret) && it != table_schema.column_end(); ++it) {
    const ObColumnSchemaV2 *col = *it;
    if (OB_ISNULL(col)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (col->is_hidden() && col->get_column_name_str().prefix_match(SPEC_PREFIX)) {
      ObAIColumnDef def;
      ObSqlString name;
      const ObColumnSchemaV2 *result = nullptr;
      const ObColumnSchemaV2 *state = nullptr;
      const ObColumnSchemaV2 *req = nullptr;
      if (OB_FAIL(def.decode(col->get_comment_str(), allocator))) {
        LOG_WARN("invalid AI column definition", K(ret), K(col->get_comment_str()));
      } else if (OB_ISNULL(result = table_schema.get_column_schema(def.column_name_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("AI result column missing", K(ret), K(def));
      } else if (OB_FAIL(name.assign_fmt("%s%.*s", STATE_PREFIX, def.column_name_.length(), def.column_name_.ptr()))) {
      } else if (OB_ISNULL(state = table_schema.get_column_schema(ObString(name.length(), name.ptr())))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(name.assign_fmt("%s%.*s", REQUEST_PREFIX, def.column_name_.length(), def.column_name_.ptr()))) {
      } else if (OB_ISNULL(req = table_schema.get_column_schema(ObString(name.length(), name.ptr())))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        def.column_id_ = result->get_column_id();
        def.state_column_id_ = state->get_column_id();
        def.request_column_id_ = req->get_column_id();
        def.spec_column_id_ = col->get_column_id();
        ret = defs.push_back(def);
      }
    }
  }
  return ret;
}

bool ObAIColumnUtil::has_ai_column(const ObTableSchema &table_schema)
{
  bool has = false;
  ObTableSchema::const_column_iterator it = table_schema.column_begin();
  for (; !has && it != table_schema.column_end(); ++it) {
    const ObColumnSchemaV2 *col = *it;
    has = OB_NOT_NULL(col) && col->is_hidden() && col->get_column_name_str().prefix_match(SPEC_PREFIX);
  }
  return has;
}

const ObAIColumnDef *ObAIColumnUtil::find_by_any_column_id(const ObIArray<ObAIColumnDef> &defs,
                                                           uint64_t column_id)
{
  const ObAIColumnDef *found = nullptr;
  for (int64_t i = 0; nullptr == found && i < defs.count(); ++i) {
    const ObAIColumnDef &def = defs.at(i);
    if (def.column_id_ == column_id || def.state_column_id_ == column_id
        || def.request_column_id_ == column_id || def.spec_column_id_ == column_id) {
      found = &def;
    }
  }
  return found;
}

int64_t ObAIColumnUtil::gen_request_id()
{
  static int64_t last_id = 0;
  int64_t now = ObTimeUtility::current_time();
  int64_t old = ATOMIC_LOAD(&last_id);
  int64_t next = 0;
  do {
    old = ATOMIC_LOAD(&last_id);
    next = (now > old) ? now : old + 1;
  } while (!ATOMIC_BCAS(&last_id, old, next));
  return next;
}

int ObAIColumnUtil::append_sql_string(ObSqlString &sql, const ObString &str)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
    const char c = str.ptr()[i];
    if ('\'' == c) {
      ret = sql.append("''");
    } else if ('\\' == c) {
      ret = sql.append("\\\\");
    } else if ('\0' == c) {
      ret = sql.append("\\0");
    } else {
      ret = sql.append(&c, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'");
  }
  return ret;
}

int ObAIColumnUtil::append_ident(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = ('`' == name.ptr()[i]) ? sql.append("``") : sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

int ObAIColumnUtil::remove_metadata(uint64_t table_id, ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s.%s WHERE table_id = %lu", OB_SYS_DATABASE_NAME,
                             share::OB_ALL_AI_COLUMN_TNAME, table_id))) {
  } else if (OB_FAIL(sql_client.write(sql.ptr(), affected))) {
    LOG_WARN("failed to remove AI column metadata", K(ret), K(sql));
  }
  return ret;
}

int ObAIColumnUtil::sync_metadata_after_ddl(const ObString &database_name, const ObString &table_name,
                                            const int64_t min_schema_version)
{
  int ret = OB_SUCCESS;
  bool done = false;
  if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  }
  // the DDL has committed; the local schema refresh may lag a little
  for (int64_t i = 0; OB_SUCC(ret) && !done && i < 100; ++i) {
    ObSchemaGetterGuard guard;
    const ObTableSchema *table_schema = nullptr;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(guard.get_table_schema(database_name, table_name, false, table_schema))) {
    } else if (OB_NOT_NULL(table_schema) && table_schema->get_schema_version() >= min_schema_version
               && has_ai_column(*table_schema)) {
      // the schema of this DDL: an earlier one misses the columns it added
      ObArenaAllocator alloc("AIColSync");
      ObSEArray<ObAIColumnDef, 4> defs;
      bool ids_ready = true;
      if (OB_FAIL(get_ai_columns(*table_schema, alloc, defs))) {
        ids_ready = false;
        ret = OB_SUCCESS;
      }
      if (ids_ready) {
        ret = sync_metadata(*table_schema, *GCTX.sql_proxy_);
        done = true;
      }
      // rows that existed before ADD AI COLUMN have no state yet: queue them
      for (int64_t j = 0; OB_SUCC(ret) && ids_ready && j < defs.count(); ++j) {
        ObSqlString cond;
        // rows written by DML carry a request id; rows that existed before ADD AI COLUMN do not
        if (OB_FAIL(cond.assign_fmt("`%s%.*s` IS NULL", REQUEST_PREFIX,
                                    defs.at(j).column_name_.length(), defs.at(j).column_name_.ptr()))) {
        } else if (OB_FAIL(retry_rows(table_schema->get_table_id(), defs.at(j).column_name_,
                                      ObString(cond.length(), cond.ptr())))) {
          LOG_WARN("failed to queue existing rows of AI column", K(ret));
        }
      }
    }
    if (OB_SUCC(ret) && !done) {
      ob_usleep(50 * 1000);
    }
  }
  if (OB_SUCC(ret) && !done) {
    LOG_WARN("AI column schema not visible after DDL", K(database_name), K(table_name));
  }
  return ret;
}

int ObAIColumnUtil::sync_metadata(const ObTableSchema &table_schema, ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColMeta");
  ObSEArray<ObAIColumnDef, 4> defs;
  if (OB_FAIL(get_ai_columns(table_schema, alloc, defs))) {
  } else if (OB_FAIL(remove_metadata(table_schema.get_table_id(), sql_client))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
    const ObAIColumnDef &def = defs.at(i);
    ObSqlString sql;
    ObSqlString normalized;
    ObSqlString deps;
    int64_t affected = 0;
    for (int64_t j = 0; OB_SUCC(ret) && j < def.deps_.count(); ++j) {
      ret = deps.append_fmt("%s%.*s", 0 == j ? "" : ",", def.deps_.at(j).length(), def.deps_.at(j).ptr());
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(def.build_normalized_expr(normalized))) {
    } else if (OB_FAIL(sql.assign_fmt("INSERT INTO %s.%s (table_id, column_id, column_name, state_column_id, "
                                      "request_column_id, result_spec_column_id, func_type, model_name, input_expr, "
                                      "normalized_expr, dependency_columns, result_type, dimension, sync_mode, "
                                      "max_retries, definition_version) VALUES (%lu, %lu, ",
                                      OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TNAME,
                                      table_schema.get_table_id(), def.column_id_))) {
    } else if (OB_FAIL(append_sql_string(sql, def.column_name_))) {
    } else if (OB_FAIL(sql.append_fmt(", %lu, %lu, %lu, %d, ", def.state_column_id_, def.request_column_id_,
                                      def.spec_column_id_, def.func_))) {
    } else if (OB_FAIL(append_sql_string(sql, def.model_))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_sql_string(sql, def.input_expr_))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_sql_string(sql, ObString(normalized.length(), normalized.ptr())))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_sql_string(sql, ObString(deps.length(), deps.ptr())))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_sql_string(sql, def.result_type_))) {
    } else if (OB_FAIL(sql.append_fmt(", %ld, ", def.dimension_))) {
    } else if (OB_FAIL(append_sql_string(sql, def.sync_mode_))) {
    } else if (OB_FAIL(sql.append_fmt(", %ld, %ld)", def.max_retries_, def.version_))) {
    } else if (OB_FAIL(sql_client.write(sql.ptr(), affected))) {
      LOG_WARN("failed to write AI column metadata", K(ret), K(sql));
    }
  }
  return ret;
}

int ObAIColumnUtil::get_full_table_name(const ObTableSchema &table_schema, ObSqlString &name)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObDatabaseSchema *db_schema = nullptr;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_database_schema(table_schema.get_database_id(), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(append_ident(name, db_schema->get_database_name_str()))) {
  } else if (OB_FAIL(name.append("."))) {
  } else if (OB_FAIL(append_ident(name, table_schema.get_table_name_str()))) {
  }
  return ret;
}

int ObAIColumnUtil::check_column_ready(const ObTableSchema &table_schema, const ObAIColumnDef &def)
{
  int ret = OB_SUCCESS;
  ObSqlString name;
  ObSqlString sql;
  bool ready = true;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(get_full_table_name(table_schema, name))) {
  } else if (OB_FAIL(sql.assign_fmt("SELECT /*+ opt_param('hidden_column_visible', 'true') */ count(*) FROM %.*s "
                                    "WHERE `%s%.*s` IS NULL OR `%s%.*s` <> %d",
                                    static_cast<int>(name.length()), name.ptr(),
                                    STATE_PREFIX, def.column_name_.length(), def.column_name_.ptr(),
                                    STATE_PREFIX, def.column_name_.length(), def.column_name_.ptr(),
                                    AI_COLUMN_STATE_READY))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = nullptr;
      int64_t cnt = 0;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to check AI column state", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else if (OB_FAIL(result->get_int(0L, cnt))) {
      } else {
        ready = (0 == cnt);
      }
    }
  }
  if (OB_SUCC(ret) && !ready) {
    ret = OB_AI_COLUMN_NOT_READY;
    LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, static_cast<int64_t>(table_schema.get_table_id()),
                   static_cast<int64_t>(def.column_id_));
  }
  return ret;
}

// In RETRY ... WHERE an unquoted STATUS names the AI state of the column;
// `status` in backticks keeps addressing a physical column.
static int rewrite_status_pseudo_column(const ObString &where, const ObString &column_name, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  const char *p = where.ptr();
  const int64_t n = where.length();
  int64_t i = 0;
  char last_sig = ' ';
  while (OB_SUCC(ret) && i < n) {
    const char c = p[i];
    if ('\'' == c || '"' == c || '`' == c) {
      int64_t j = i + 1;
      while (j < n) {
        if ('\\' == p[j] && '`' != c) {
          j += 2;
        } else if (p[j] == c) {
          if (j + 1 < n && p[j + 1] == c) {
            j += 2;
          } else {
            break;
          }
        } else {
          ++j;
        }
      }
      j = MIN(j + 1, n);
      ret = out.append(p + i, j - i);
      last_sig = c;
      i = j;
    } else if (isalnum(static_cast<unsigned char>(c)) || '_' == c || '$' == c) {
      int64_t j = i;
      while (j < n && (isalnum(static_cast<unsigned char>(p[j])) || '_' == p[j] || '$' == p[j])) {
        ++j;
      }
      ObString word(static_cast<int32_t>(j - i), p + i);
      int64_t k = j;
      while (k < n && isspace(static_cast<unsigned char>(p[k]))) {
        ++k;
      }
      const bool qualified = ('.' == last_sig) || (k < n && ('.' == p[k] || '(' == p[k]));
      if (!qualified && 0 == word.case_compare("status")) {
        ret = out.append_fmt("(CASE `%s%.*s` WHEN %d THEN 'READY' WHEN %d THEN 'FAILED' ELSE 'PENDING' END)",
                             ObAIColumnUtil::STATE_PREFIX, column_name.length(), column_name.ptr(),
                             AI_COLUMN_STATE_READY, AI_COLUMN_STATE_FAILED);
      } else {
        ret = out.append(word);
      }
      last_sig = 'a';
      i = j;
    } else {
      ret = out.append(p + i, 1);
      if (!isspace(static_cast<unsigned char>(c))) {
        last_sig = c;
      }
      ++i;
    }
  }
  return ret;
}

int ObAIColumnUtil::retry_rows(uint64_t table_id, const ObString &column_name, const ObString &where)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColRetry");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table_schema = nullptr;
  ObSEArray<ObAIColumnDef, 4> defs;
  const ObAIColumnDef *def = nullptr;
  ObSqlString name;
  ObSqlString sql;
  ObSqlString cond;
  ObSEArray<ObString, 16> preds;
  ObSEArray<const ObColumnSchemaV2 *, 4> pk_cols;
  if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(get_ai_columns(*table_schema, alloc, defs))) {
  } else if (OB_FAIL(get_full_table_name(*table_schema, name))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && nullptr == def && i < defs.count(); ++i) {
    if (0 == defs.at(i).column_name_.case_compare(column_name)) {
      def = &defs.at(i);
    }
  }
  if (OB_SUCC(ret) && OB_ISNULL(def)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schema->get_rowkey_info().get_size(); ++i) {
    uint64_t cid = OB_INVALID_ID;
    const ObColumnSchemaV2 *col = nullptr;
    if (OB_FAIL(table_schema->get_rowkey_info().get_column_id(i, cid))) {
    } else if (OB_ISNULL(col = table_schema->get_column_schema(cid))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ret = pk_cols.push_back(col);
    }
  }
  // 1. rows selected by the predicate
  if (OB_FAIL(ret)) {
  } else if (!where.empty() && OB_FAIL(rewrite_status_pseudo_column(where, def->column_name_, cond))) {
  } else if (OB_FAIL(sql.assign("SELECT /*+ opt_param('hidden_column_visible', 'true') */ "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < pk_cols.count(); ++i) {
    if (OB_FAIL(sql.append(0 == i ? "CAST(" : ", CAST("))) {
    } else if (OB_FAIL(append_ident(sql, pk_cols.at(i)->get_column_name_str()))) {
    } else if (OB_FAIL(sql.append(" AS CHAR)"))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append_fmt(" FROM %.*s", static_cast<int>(name.length()), name.ptr()))) {
  } else if (!cond.empty()
             && OB_FAIL(sql.append_fmt(" WHERE (%.*s)", static_cast<int>(cond.length()), cond.ptr()))) {
  }
  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to select rows to retry", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret)) {
        ObSqlString pred;
        ObString copied;
        if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < pk_cols.count(); ++i) {
          ObString v;
          if (OB_FAIL(result->get_varchar(i, v))) {
          } else if (OB_FAIL(pred.append(0 == i ? "" : " AND "))) {
          } else if (OB_FAIL(append_ident(pred, pk_cols.at(i)->get_column_name_str()))) {
          } else if (OB_FAIL(pred.append(" = "))) {
          } else if (ob_is_integer_type(pk_cols.at(i)->get_data_type())) {
            ret = pred.append(v);
          } else {
            ret = append_sql_string(pred, v);
          }
        }
        if (OB_SUCC(ret) && OB_SUCC(ob_write_string(alloc, ObString(pred.length(), pred.ptr()), copied))) {
          ret = preds.push_back(copied);
        }
      }
    }
  }
  // 2. invalidate the selected rows and queue new requests, in one transaction
  if (OB_SUCC(ret) && preds.count() > 0) {
    ObMySQLTransaction trans;
    if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < preds.count(); ++i) {
      const ObString &pred = preds.at(i);
      const int64_t req = gen_request_id();
      int64_t affected = 0;
      if (OB_FAIL(sql.assign_fmt("DELETE FROM %s.%s WHERE table_id = %lu AND column_id = %lu AND rowkey_predicate = ",
                                 OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME, table_id, def->column_id_))) {
      } else if (OB_FAIL(append_sql_string(sql, pred))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
      } else if (OB_FAIL(sql.assign_fmt("UPDATE /*+ opt_param('hidden_column_visible', 'true') */ %.*s SET ",
                                        static_cast<int>(name.length()), name.ptr()))) {
      } else if (OB_FAIL(append_ident(sql, def->column_name_))) {
      } else if (OB_FAIL(sql.append_fmt(" = NULL, `%s%.*s` = %d, `%s%.*s` = %ld WHERE %.*s",
                                        STATE_PREFIX, def->column_name_.length(), def->column_name_.ptr(),
                                        AI_COLUMN_STATE_PENDING,
                                        REQUEST_PREFIX, def->column_name_.length(), def->column_name_.ptr(), req,
                                        pred.length(), pred.ptr()))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
        LOG_WARN("failed to invalidate AI column row", K(ret), K(sql));
      } else if (0 == affected) {
      } else if (OB_FAIL(sql.assign_fmt("INSERT INTO %s.%s (table_id, request_id, column_id, rowkey_predicate, state, "
                                        "retry_count, eligible_ts, definition_version) VALUES (%lu, %ld, %lu, ",
                                        OB_SYS_DATABASE_NAME, share::OB_ALL_AI_COLUMN_TASK_TNAME,
                                        table_id, req, def->column_id_))) {
      } else if (OB_FAIL(append_sql_string(sql, pred))) {
      } else if (OB_FAIL(sql.append_fmt(", %d, 0, now(6), %ld)", AI_TASK_STATE_PENDING, def->version_))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
      }
    }
    if (trans.is_started()) {
      int tmp_ret = trans.end(OB_SUCCESS == ret);
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
