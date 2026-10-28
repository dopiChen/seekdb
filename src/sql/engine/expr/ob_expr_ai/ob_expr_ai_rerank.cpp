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

namespace
{

// One score returned by the model.  The index is the position of the document
// inside the request the score belongs to; it is made global before sorting.
struct ObRerankItem
{
  int64_t index_;
  double score_;
  ObJsonObject *result_obj_;
  TO_STRING_KV(K_(index), K_(score));
};

// Providers do not agree on the name of the score field.
int get_rerank_score(ObJsonObject *result_obj, double &score)
{
  int ret = OB_SUCCESS;
  static const char *SCORE_KEYS[] = {"relevance_score", "score"};
  bool found = false;
  score = 0;
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < ARRAYSIZEOF(SCORE_KEYS); ++i) {
    ObJsonNode *score_node = result_obj->get_value(SCORE_KEYS[i]);
    if (OB_ISNULL(score_node)) {
      continue;
    } else if (score_node->json_type() == ObJsonNodeType::J_DOUBLE) {
      score = static_cast<ObJsonDouble *>(score_node)->value();
      found = true;
    } else if (score_node->json_type() == ObJsonNodeType::J_INT) {
      score = static_cast<double>(static_cast<ObJsonInt *>(score_node)->get_int());
      found = true;
    } else {
      ret = OB_INVALID_DATA;
      LOG_WARN("the rerank score is not a number", K(ret), K(i));
    }
  }
  if (OB_SUCC(ret) && !found) {
    ret = OB_INVALID_DATA;
    LOG_WARN("the rerank result has no score", K(ret));
  }
  return ret;
}

// Every document has to be scored exactly once.  The response is rejected when
// it is malformed instead of producing a partial or misaligned result.
int parse_rerank_items(ObJsonArray *model_result_array,
                       const int64_t doc_count,
                       ObArray<ObRerankItem> &items)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(model_result_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (model_result_array->element_count() != doc_count) {
    ret = OB_INVALID_DATA;
    LOG_WARN("the rerank result count does not match the document count",
             K(ret), K(model_result_array->element_count()), K(doc_count));
  } else {
    ObArray<bool> visited;
    for (int64_t i = 0; OB_SUCC(ret) && i < doc_count; ++i) {
      if (OB_FAIL(visited.push_back(false))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < doc_count; ++i) {
      ObJsonNode *result_node = model_result_array->get_value(i);
      if (OB_ISNULL(result_node) || result_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("the rerank result is not an object", K(ret), K(i));
      } else {
        ObJsonObject *result_obj = static_cast<ObJsonObject *>(result_node);
        ObJsonNode *index_node = result_obj->get_value("index");
        double score = 0;
        if (OB_ISNULL(index_node) || index_node->json_type() != ObJsonNodeType::J_INT) {
          ret = OB_INVALID_DATA;
          LOG_WARN("the rerank result has no valid index", K(ret), K(i));
        } else if (OB_FAIL(get_rerank_score(result_obj, score))) {
        } else {
          const int64_t index = static_cast<ObJsonInt *>(index_node)->get_int();
          if (index < 0 || index >= doc_count) {
            ret = OB_INVALID_DATA;
            LOG_WARN("the rerank index is out of range", K(ret), K(index), K(doc_count));
          } else if (visited.at(index)) {
            ret = OB_INVALID_DATA;
            LOG_WARN("the rerank index is duplicated", K(ret), K(index));
          } else {
            visited.at(index) = true;
            ObRerankItem item;
            item.index_ = index;
            item.score_ = score;
            item.result_obj_ = result_obj;
            if (OB_FAIL(items.push_back(item))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

bool rerank_item_is_greater(const ObRerankItem &left, const ObRerankItem &right)
{
  if (left.score_ != right.score_) {
    return left.score_ > right.score_;
  }
  // Stable ordering: documents sharing a score keep their original order.
  return left.index_ < right.index_;
}

void sort_rerank_items(ObArray<ObRerankItem> &items)
{
  for (int64_t i = 1; i < items.count(); ++i) {
    ObRerankItem item = items.at(i);
    int64_t j = i - 1;
    while (j >= 0 && rerank_item_is_greater(item, items.at(j))) {
      items.at(j + 1) = items.at(j);
      --j;
    }
    items.at(j + 1) = item;
  }
}

} // namespace

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
                                    common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4 || OB_ISNULL(types_stack))) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_type(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_type(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    const ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      const ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
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

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    // A NULL or empty doc key is the same as no doc key: documents are strings.
    const bool has_doc_key = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null()
                             && !arg_doc_key->get_string().empty();
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (is_null_result || OB_ISNULL(j_base)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("the documents argument is null", K(ret));
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(
                   temp_allocator, *info, resolved_endpoint, header_array))) {
    }

    ObJsonArray *doc_array = nullptr;
    if (OB_FAIL(ret)) {
    } else if (has_doc_key) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(get_doc_array_from_documents_array_with_key(
              temp_allocator, document_array, doc_key, doc_array))) {
      }
    } else {
      doc_array = document_array;
    }

    ObArray<ObRerankItem> all_items;
    if (OB_FAIL(ret)) {
    } else {
      const int64_t doc_count = doc_array->element_count();
      for (int64_t begin = 0; OB_SUCC(ret) && begin < doc_count; begin += RERANK_BATCH_SIZE) {
        const int64_t end = MIN(begin + RERANK_BATCH_SIZE, doc_count);
        ObJsonArray *batch_document_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        ObArray<ObRerankItem> batch_items;
        if (OB_FAIL(construct_batch_document_array(
                temp_allocator, doc_array, begin, end, batch_document_array))) {
        } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, resolved_endpoint,
                       header_array, query, batch_document_array, batch_result_array))) {
        } else if (OB_FAIL(parse_rerank_items(batch_result_array, end - begin, batch_items))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < batch_items.count(); ++i) {
            ObRerankItem item = batch_items.at(i);
            item.index_ += begin;
            if (!has_doc_key) {
              ObJsonInt *index_obj = static_cast<ObJsonInt *>(
                  item.result_obj_->get_value(INDEX_KEY));
              if (OB_ISNULL(index_obj)) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                index_obj->set_value(item.index_);
              }
            }
            if (OB_SUCC(ret) && OB_FAIL(all_items.push_back(item))) {
            }
          }
        }
      }
    }

    ObJsonArray *result_array = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, result_array))) {
    } else {
      sort_rerank_items(all_items);
      for (int64_t i = 0; OB_SUCC(ret) && i < all_items.count(); ++i) {
        const ObRerankItem &item = all_items.at(i);
        if (has_doc_key) {
          ObJsonNode *origin_node = document_array->get_value(item.index_);
          ObJsonObject *origin_obj = nullptr;
          ObJsonObject *new_obj = nullptr;
          if (OB_ISNULL(origin_node) || origin_node->json_type() != ObJsonNodeType::J_OBJECT
              || OB_ISNULL(origin_obj = static_cast<ObJsonObject *>(origin_node))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(
                         origin_obj->clone(&temp_allocator, true)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            ObJsonDouble *score_obj = OB_NEWx(ObJsonDouble, &temp_allocator, item.score_);
            if (OB_ISNULL(score_obj)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(new_obj->add(MODEL_SCORE_KEY, score_obj))) {
            } else if (OB_FAIL(result_array->append(new_obj))) {
            }
          }
        } else if (OB_ISNULL(item.result_obj_)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result_array->append(item.result_obj_))) {
        }
      }
    }

    if (OB_SUCC(ret)) {
      ObString raw_str;
      if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator,
                                                               ObJsonArray *document_object_array,
                                                               ObString &doc_key,
                                                               ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      const int64_t count = document_object_array->element_count();
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        ObJsonNode *doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          ObJsonObject *doc_obj = static_cast<ObJsonObject *>(doc_node);
          ObJsonNode *doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator,
                                                   ObJsonArray *document_array,
                                                   int64_t start_idx,
                                                   int64_t end_idx,
                                                   ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; OB_SUCC(ret) && i < end_idx; ++i) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator,
                                         const ObAIFuncExprInfo &info,
                                         const share::ObAiModelEndpointInfo &endpoint_info,
                                         ObArray<ObString> &header_array,
                                         ObString &query,
                                         ObJsonArray *document_array,
                                         ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(
          allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(
                 allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(
                 allocator, endpoint_info, http_response, response))) {
  } else if (OB_ISNULL(response) || response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
    LOG_WARN("the rerank response is not an array", K(ret));
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx,
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  // TODO: support schema version match in plan cache for ai func
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
