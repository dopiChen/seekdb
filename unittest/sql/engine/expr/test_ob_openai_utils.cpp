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

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include "lib/allocator/page_arena.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace sql
{

// Unit tests of the provider protocols: the embedding response is rebuilt by
// the index member so that an out of order answer cannot shift the rows, and
// the native Ollama requests follow the /api/generate and /api/embed protocol.
class TestObOpenAIUtils : public ::testing::Test
{
public:
  static ObString make_string(const char *text)
  {
    return ObString(static_cast<int32_t>(strlen(text)), text);
  }

  static int first_value(ObIJsonBase *result, const int64_t position, double &value)
  {
    int ret = OB_SUCCESS;
    ObJsonArray *result_array = nullptr;
    ObJsonNode *vector_node = nullptr;
    ObJsonNode *value_node = nullptr;
    if (OB_ISNULL(result) || result->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(result_array = static_cast<ObJsonArray *>(result))) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(vector_node = result_array->get_value(position))
               || vector_node->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_DATA;
    } else if (OB_ISNULL(value_node = static_cast<ObJsonArray *>(vector_node)->get_value(0))) {
      ret = OB_INVALID_DATA;
    } else if (value_node->json_type() == ObJsonNodeType::J_DOUBLE) {
      value = static_cast<ObJsonDouble *>(value_node)->get_double();
    } else if (value_node->json_type() == ObJsonNodeType::J_INT) {
      value = static_cast<double>(static_cast<ObJsonInt *>(value_node)->get_int());
    } else {
      ret = OB_INVALID_DATA;
    }
    return ret;
  }

  static int parse_embed_response(
      ObIAllocator &allocator, const char *response_text, ObIJsonBase *&result)
  {
    int ret = OB_SUCCESS;
    ObString text = make_string(response_text);
    ObJsonObject *http_response = nullptr;
    ObOpenAIUtils::ObOpenAIEmbed embed_provider;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator, text, http_response))) {
    } else if (OB_FAIL(embed_provider.parse_output(allocator, http_response, result))) {
    }
    return ret;
  }

  static bool has_header(const ObArray<ObString> &headers, const char *expected)
  {
    bool found = false;
    for (int64_t i = 0; !found && i < headers.count(); ++i) {
      found = headers.at(i).case_compare(expected) == 0;
    }
    return found;
  }

protected:
  common::ObArenaAllocator allocator_;
};

TEST_F(TestObOpenAIUtils, embed_output_is_rebuilt_by_index)
{
  const char *response =
      "{\"data\":[{\"index\":1,\"embedding\":[1.0,1.5]},"
      "{\"index\":0,\"embedding\":[0.0,0.5]}]}";
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_embed_response(allocator_, response, result));
  double value = -1.0;
  ASSERT_EQ(OB_SUCCESS, first_value(result, 0, value));
  EXPECT_DOUBLE_EQ(0.0, value);
  ASSERT_EQ(OB_SUCCESS, first_value(result, 1, value));
  EXPECT_DOUBLE_EQ(1.0, value);
}

TEST_F(TestObOpenAIUtils, embed_output_without_index_keeps_response_order)
{
  const char *response =
      "{\"data\":[{\"embedding\":[0.0]},{\"embedding\":[1.0]}]}";
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_embed_response(allocator_, response, result));
  double value = -1.0;
  ASSERT_EQ(OB_SUCCESS, first_value(result, 0, value));
  EXPECT_DOUBLE_EQ(0.0, value);
  ASSERT_EQ(OB_SUCCESS, first_value(result, 1, value));
  EXPECT_DOUBLE_EQ(1.0, value);
}

TEST_F(TestObOpenAIUtils, duplicated_index_is_rejected)
{
  const char *response =
      "{\"data\":[{\"index\":0,\"embedding\":[0.0]},{\"index\":0,\"embedding\":[1.0]}]}";
  ObIJsonBase *result = nullptr;
  EXPECT_NE(OB_SUCCESS, parse_embed_response(allocator_, response, result));
}

TEST_F(TestObOpenAIUtils, index_out_of_range_is_rejected)
{
  const char *response =
      "{\"data\":[{\"index\":5,\"embedding\":[0.0]},{\"index\":0,\"embedding\":[1.0]}]}";
  ObIJsonBase *result = nullptr;
  EXPECT_NE(OB_SUCCESS, parse_embed_response(allocator_, response, result));
}

TEST_F(TestObOpenAIUtils, missing_embedding_is_rejected)
{
  const char *response =
      "{\"data\":[{\"index\":0,\"embedding\":[0.0]},{\"index\":1}]}";
  ObIJsonBase *result = nullptr;
  EXPECT_NE(OB_SUCCESS, parse_embed_response(allocator_, response, result));
}

TEST_F(TestObOpenAIUtils, empty_data_is_allowed)
{
  const char *response = "{\"data\":[]}";
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_embed_response(allocator_, response, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  EXPECT_EQ(0, static_cast<ObJsonArray *>(result)->element_count());
}

TEST_F(TestObOpenAIUtils, non_array_data_is_rejected)
{
  const char *response = "{\"data\":{}}";
  ObIJsonBase *result = nullptr;
  EXPECT_NE(OB_SUCCESS, parse_embed_response(allocator_, response, result));
}

TEST_F(TestObOpenAIUtils, ollama_embed_output_is_the_native_embeddings_array)
{
  const char *response = "{\"embeddings\":[[0.0,0.5],[1.0,1.5]]}";
  ObString text = make_string(response);
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *result = nullptr;
  ObOllamaUtils::ObOllamaEmbed embed_provider;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, text, http_response));
  ASSERT_EQ(OB_SUCCESS, embed_provider.parse_output(allocator_, http_response, result));
  double value = -1.0;
  ASSERT_EQ(OB_SUCCESS, first_value(result, 0, value));
  EXPECT_DOUBLE_EQ(0.0, value);
  ASSERT_EQ(OB_SUCCESS, first_value(result, 1, value));
  EXPECT_DOUBLE_EQ(1.0, value);
}

TEST_F(TestObOpenAIUtils, ollama_headers_are_json)
{
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::get_header(allocator_, headers));
  EXPECT_TRUE(has_header(headers, "Content-Type: application/json"));
}

TEST_F(TestObOpenAIUtils, ollama_completion_body_is_not_streamed)
{
  ObString model = make_string("qwen2.5");
  ObString prompt = make_string("describe seekdb");
  ObJsonObject *config = nullptr;
  ObJsonObject *body = nullptr;
  ObOllamaUtils::ObOllamaComplete complete_provider;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ASSERT_EQ(OB_SUCCESS, complete_provider.get_body(
      allocator_, model, prompt, prompt, config, body));
  ASSERT_NE(nullptr, body);
  ObJsonNode *stream_node = body->get_value("stream");
  ASSERT_NE(nullptr, stream_node);
  ASSERT_EQ(ObJsonNodeType::J_BOOLEAN, stream_node->json_type());
  EXPECT_FALSE(static_cast<ObJsonBoolean *>(stream_node)->value());
  ASSERT_NE(nullptr, body->get_value("model"));
  ASSERT_NE(nullptr, body->get_value("prompt"));
}

TEST_F(TestObOpenAIUtils, ollama_embedding_body_carries_every_input)
{
  ObString model = make_string("nomic-embed-text");
  ObArray<ObString> contents;
  ObJsonObject *config = nullptr;
  ObJsonObject *body = nullptr;
  ObString first = make_string("first");
  ObString second = make_string("second");
  ASSERT_EQ(OB_SUCCESS, contents.push_back(first));
  ASSERT_EQ(OB_SUCCESS, contents.push_back(second));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ObOllamaUtils::ObOllamaEmbed embed_provider;
  ASSERT_EQ(OB_SUCCESS, embed_provider.get_body(allocator_, model, contents, config, body));
  ASSERT_NE(nullptr, body);
  ObJsonNode *input_node = body->get_value("input");
  ASSERT_NE(nullptr, input_node);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, input_node->json_type());
  EXPECT_EQ(2, static_cast<ObJsonArray *>(input_node)->element_count());
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
