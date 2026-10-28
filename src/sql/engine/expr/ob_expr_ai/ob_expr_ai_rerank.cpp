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
#include "ob_expr_ai_rerank.h"
#include <algorithm>
#include <cmath>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_type(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

namespace {
struct RankedAIResult
{
  int64_t index_;
  double score_;
  ObJsonObject *value_;
};
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *model_datum = nullptr;
  ObDatum *query_datum = nullptr;
  ObDatum *documents_datum = nullptr;
  ObDatum *key_datum = nullptr;
  if (expr.arg_cnt_ == 4
      ? OB_FAIL(expr.eval_param_value(ctx, model_datum, query_datum, documents_datum, key_datum))
      : OB_FAIL(expr.eval_param_value(ctx, model_datum, query_datum, documents_datum))) {
  } else if (model_datum->is_null() || query_datum->is_null() || documents_datum->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObEvalCtx::TempAllocGuard tmp_guard(ctx);
    MultimodeAlloctor allocator(tmp_guard.get_allocator());
    ObString model_id = model_datum->get_string();
    ObString query;
    ObString document_key;
    const bool object_documents = OB_NOT_NULL(key_datum) && !key_datum->is_null();
    ObIJsonBase *base = nullptr;
    ObJsonArray *documents = nullptr;
    ObJsonArray *texts = nullptr;
    bool is_null = false;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
            *query_datum, expr.args_[QUERY_IDX]->datum_meta_,
            expr.args_[QUERY_IDX]->obj_meta_.has_lob_header(), query))) {
    } else if (object_documents && OB_FAIL(ObTextStringHelper::read_real_string_data(
                   ctx.exec_ctx_, allocator, *key_datum, expr.args_[DOC_KEY_IDX]->datum_meta_,
                   expr.args_[DOC_KEY_IDX]->obj_meta_.has_lob_header(), document_key))) {
    } else if (model_id.empty() || query.empty() || (object_documents && document_key.empty())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator, DOCUMENTS_IDX, base, is_null))) {
    } else if (is_null || OB_ISNULL(base) || base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FALSE_IT(documents = static_cast<ObJsonArray *>(base))) {
    } else if (documents->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, texts))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < documents->element_count(); ++i) {
        ObJsonNode *document = documents->get_value(i);
        ObJsonNode *text = document;
        if (OB_ISNULL(document)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (object_documents) {
          if (document->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            text = static_cast<ObJsonObject *>(document)->get_value(document_key);
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_ISNULL(text) || text->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(texts->append(text))) {
        }
      }
    }

    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    query::ObIAiEndpointResolver *resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
    }

    ObArray<RankedAIResult> ranked;
    const int64_t batch_size = 20;
    if (OB_SUCC(ret)) {
      ObAIFuncModel model(allocator, *info, endpoint);
      for (int64_t start = 0; OB_SUCC(ret) && start < texts->element_count(); start += batch_size) {
        const int64_t count = std::min(batch_size, static_cast<int64_t>(texts->element_count()) - start);
        ObJsonArray *batch = nullptr;
        ObJsonArray *response = nullptr;
        ObArray<bool> seen;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, batch))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          if (OB_FAIL(batch->append(texts->get_value(start + i)))) {
          } else if (OB_FAIL(seen.push_back(false))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(model.call_rerank(query, batch, response))) {
        } else if (OB_ISNULL(response) || response->element_count() != count) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
            ObJsonNode *node = response->get_value(i);
            ObJsonNode *index_node = nullptr;
            ObJsonNode *score_node = nullptr;
            int64_t index = -1;
            double score = 0;
            if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
              ret = OB_INVALID_DATA;
            } else {
              ObJsonObject *value = static_cast<ObJsonObject *>(node);
              index_node = value->get_value(INDEX_KEY);
              score_node = value->get_value(SCORE_KEY);
              if (OB_ISNULL(index_node) || OB_ISNULL(score_node) || !score_node->is_number()) {
                ret = OB_INVALID_DATA;
              } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
                index = index_node->get_int();
              } else if (index_node->json_type() == ObJsonNodeType::J_UINT && index_node->get_uint() < static_cast<uint64_t>(count)) {
                index = static_cast<int64_t>(index_node->get_uint());
              } else {
                ret = OB_INVALID_DATA;
              }
              if (OB_FAIL(ret)) {
              } else if (index < 0 || index >= count || seen.at(index)) {
                ret = OB_INVALID_DATA;
              } else if (OB_FAIL(score_node->to_double(score))) {
              } else if (!std::isfinite(score)) {
                ret = OB_INVALID_DATA;
              } else {
                seen.at(index) = true;
                RankedAIResult result = {start + index, score, value};
                if (OB_FAIL(ranked.push_back(result))) {
                }
              }
            }
          }
        }
      }
    }

    ObJsonArray *output = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, output))) {
    } else {
      // Original document order resolves ties, independent of response order or
      // batch boundaries. No ordering assumptions are made about a provider.
      std::sort(ranked.begin(), ranked.end(), [](const RankedAIResult &a, const RankedAIResult &b) {
        return a.score_ == b.score_ ? a.index_ < b.index_ : a.score_ > b.score_;
      });
      for (int64_t i = 0; OB_SUCC(ret) && i < ranked.count(); ++i) {
        const RankedAIResult &result = ranked.at(i);
        ObJsonObject *object = nullptr;
        ObJsonNode *origin = object_documents ? documents->get_value(result.index_) : result.value_;
        if (OB_ISNULL(object = static_cast<ObJsonObject *>(origin->clone(&allocator, true)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (object_documents) {
          ObJsonDouble *score_json = nullptr;
          if (OB_ISNULL(score_json = OB_NEWx(ObJsonDouble, &allocator, result.score_))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(object->add(MODEL_SCORE_KEY, score_json))) {
          }
        } else {
          ObJsonInt *index_json = nullptr;
          if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, result.index_, index_json))) {
          } else if (OB_FAIL(object->add(INDEX_KEY, index_json))) {
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(output->append(object))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObString raw;
      if (OB_FAIL(output->get_raw_binary(raw, &allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
