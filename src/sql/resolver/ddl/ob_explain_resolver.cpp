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
#include <string>
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
namespace
{
// Escape a string so that it can be used inside a single quoted SQL literal.
void append_sql_escaped(std::string &out, const std::string &in)
{
  for (std::string::const_iterator it = in.begin(); it != in.end(); ++it) {
    if ('\'' == *it) {
      out.append("''");
    } else {
      out.push_back(*it);
    }
  }
}

void append_file_column_type(std::string &out, int32_t type, int64_t varchar_length)
{
  (void)varchar_length;
  switch (type) {
    case static_cast<int32_t>(file_sql::FT_BOOLEAN):
      out.append("BOOLEAN");
      break;
    case static_cast<int32_t>(file_sql::FT_BIGINT):
      out.append("BIGINT");
      break;
    case static_cast<int32_t>(file_sql::FT_DOUBLE):
      out.append("DOUBLE");
      break;
    case static_cast<int32_t>(file_sql::FT_DATE):
      out.append("DATE");
      break;
    case static_cast<int32_t>(file_sql::FT_DATETIME):
      out.append("DATETIME");
      break;
    case static_cast<int32_t>(file_sql::FT_VARCHAR):
      out.append("VARCHAR");
      break;
    case static_cast<int32_t>(file_sql::FT_NULL):
    default:
      out.append("NULL");
      break;
  }
}
} // namespace

bool ObExplainResolver::is_file_table_select(ObStmt &child_stmt)
{
  bool is_file_table = false;
  if (child_stmt.is_select_stmt()) {
    ObSelectStmt &select_stmt = static_cast<ObSelectStmt&>(child_stmt);
    ObIArray<TableItem*> &table_items = select_stmt.get_table_items();
    if (1 == table_items.count() && OB_NOT_NULL(table_items.at(0))
        && table_items.at(0)->is_file_table()
        && OB_NOT_NULL(table_items.at(0)->file_table_def_)) {
      is_file_table = true;
    }
  }
  return is_file_table;
}

int ObExplainResolver::build_file_table_describe_stmt(ObStmt &child_stmt, ObStmt *&desc_stmt)
{
  int ret = OB_SUCCESS;
  desc_stmt = NULL;
  if (OB_UNLIKELY(!is_file_table_select(child_stmt))) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "DESCRIBE statement on non file table query");
  } else {
    ObSelectStmt &select_stmt = static_cast<ObSelectStmt&>(child_stmt);
    TableItem *table_item = select_stmt.get_table_items().at(0);
    const ObFileTableDef *table_def = table_item->file_table_def_;
    const int64_t column_count = table_def->columns_.count();
    if (OB_UNLIKELY(column_count <= 0)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected empty file table schema", K(ret));
    } else {
      std::string sql;
      sql.reserve(128 + 96 * static_cast<size_t>(column_count));
      for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
        const ObFileColumnDef &col_def = table_def->columns_.at(i);
        std::string col_name(col_def.column_name_.ptr(), col_def.column_name_.length());
        std::string type_str;
        append_file_column_type(type_str, col_def.type_, col_def.varchar_length_);
        if (i > 0) {
          sql.append(" UNION ALL ");
        }
        sql.append("SELECT CAST('");
        append_sql_escaped(sql, col_name);
        sql.append("' AS CHAR(256)) AS column_name, CAST('");
        append_sql_escaped(sql, type_str);
        sql.append("' AS CHAR(64)) AS column_type, CAST('");
        sql.append(col_def.nullable_ ? "YES" : "NO");
        sql.append("' AS CHAR(3)) AS `null`, CAST(NULL AS CHAR(4)) AS `key`, "
                   "CAST(NULL AS CHAR(8)) AS `default`, CAST(NULL AS CHAR(4)) AS extra");
      }
      if (OB_SUCC(ret)) {
        ParseResult parse_result;
        ObParser parser(*params_.allocator_,
                        params_.session_info_->get_sql_mode(),
                        params_.session_info_->get_charsets4parser());
        ObString sql_str(static_cast<int32_t>(sql.length()), sql.c_str());
        if (OB_FAIL(parser.parse(sql_str, parse_result))) {
          LOG_WARN("failed to parse describe file table sql", K(ret), K(sql_str));
        } else if (OB_ISNULL(parse_result.result_tree_)
                   || parse_result.result_tree_->num_child_ < 1
                   || OB_ISNULL(parse_result.result_tree_->children_[0])) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected empty parse tree for describe", K(ret));
        } else {
          ObResolver resolver(params_);
          if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                       *parse_result.result_tree_->children_[0],
                                       desc_stmt))) {
            LOG_WARN("failed to resolve describe file table sql", K(ret));
          } else if (OB_ISNULL(desc_stmt)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected null statement for describe", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
    const bool is_describe =
      (NULL != format && T_INT == format->type_ && 1 == format->value_);
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else if (is_describe && is_file_table_select(*child_stmt)) {
        ObStmt *desc_stmt = NULL;
        if (OB_FAIL(build_file_table_describe_stmt(*child_stmt, desc_stmt))) {
        } else if (OB_ISNULL(desc_stmt)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          // Replace the DESCRIBE statement with the metadata query so that the
          // normal SELECT execution path produces the dynamic column metadata.
          stmt_ = desc_stmt;
        }
      } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
        ret = OB_SQL_RESOLVER_NO_MEMORY;
        LOG_WARN("failed to create explain stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        if (NULL != format) {
          switch (format->type_) {
          case T_INT:
            // DESCRIBE/DESC marker, not a format option
            break;
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    if (OB_SUCC(ret) && NULL != explain_stmt) {
      ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
      if (NULL != opt_node) {
        switch (opt_node->type_) {
        case T_PRETTY_COLOR:
          opt.with_color_ = true;
          // pass
        case T_PRETTY:
          opt.with_tree_line_ = true;
          break;
        default:
          ret = OB_ERR_UNEXPECTED;
        }
      }
      if (OB_SUCC(ret)) {
        explain_stmt->set_display_opt(opt);
      }
      ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
      if (OB_SUCC(ret) && NULL != into_table_node) {
        ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
        explain_stmt->set_into_table(into_table);
      }
      ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
      if (OB_SUCC(ret) && NULL != statement_id_node) {
        ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
        explain_stmt->set_statement_id(statement_id);
      }
    }
  }

  return ret;
}
