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
#include <initializer_list>
#include "lib/allocator/page_arena.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace sql
{

// Unit tests of the prompt object rendering.  The SQL level behaviour is
// covered by tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test,
// these cases pin down the rendering rules of ObAIFuncPromptObjectUtils.
class TestObExprAIPrompt : public ::testing::Test
{
public:
  static ObString make_string(const char *text)
  {
    return ObString(static_cast<int32_t>(strlen(text)), text);
  }

  static std::string to_std_string(const ObString &str)
  {
    return std::string(str.ptr(), str.length());
  }

  static void build_str_args(ObIAllocator &allocator,
                             std::initializer_list<const char *> args,
                             ObJsonArray *&args_array)
  {
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator, args_array));
    for (const char *arg : args) {
      ObString arg_str = make_string(arg);
      ObJsonString *arg_node = nullptr;
      ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator, arg_str, arg_node));
      ASSERT_EQ(OB_SUCCESS, args_array->append(arg_node));
    }
  }

  static int append_json_arg(ObIAllocator &allocator, ObJsonArray *args_array, const char *text)
  {
    int ret = OB_SUCCESS;
    ObString text_str = make_string(text);
    ObJsonObject *obj_node = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator, text_str, obj_node))) {
    } else if (OB_FAIL(args_array->append(obj_node))) {
    }
    return ret;
  }

  static int render(ObIAllocator &allocator,
                    const char *template_text,
                    ObJsonArray *args_array,
                    std::string &rendered)
  {
    int ret = OB_SUCCESS;
    ObString template_str = make_string(template_text);
    ObJsonObject *prompt_object = nullptr;
    ObString replaced;
    if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(
                allocator, template_str, args_array, prompt_object))) {
    } else if (OB_FAIL(ObAIFuncPromptObjectUtils::replace_all_args_in_template(
                       allocator, prompt_object, replaced))) {
    } else {
      rendered = to_std_string(replaced);
    }
    return ret;
  }

protected:
  common::ObArenaAllocator allocator_;
};

TEST_F(TestObExprAIPrompt, replaces_single_placeholder)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  build_str_args(allocator_, {"1"}, args_array);
  // Placeholders without an argument stay literal so that a template can be
  // extended later, which is the behaviour the SQL tests rely on.
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "{0}+{1}={2}", args_array, rendered));
  EXPECT_EQ("1+{1}={2}", rendered);
}

TEST_F(TestObExprAIPrompt, replaces_repeated_placeholder)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  build_str_args(allocator_, {"x"}, args_array);
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "{0}-{0}-{0}", args_array, rendered));
  EXPECT_EQ("x-x-x", rendered);
}

TEST_F(TestObExprAIPrompt, replaces_whole_template)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  build_str_args(allocator_, {"world"}, args_array);
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "{0}", args_array, rendered));
  EXPECT_EQ("world", rendered);
}

TEST_F(TestObExprAIPrompt, escaped_braces_are_literal)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  build_str_args(allocator_, {"x"}, args_array);
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "{{0}} {0}", args_array, rendered));
  EXPECT_EQ("{0} x", rendered);
}

TEST_F(TestObExprAIPrompt, non_placeholder_braces_are_literal)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  build_str_args(allocator_, {"x"}, args_array);
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "{name} {0} {1", args_array, rendered));
  EXPECT_EQ("{name} x {1", rendered);
}

TEST_F(TestObExprAIPrompt, unicode_argument_is_preserved)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  build_str_args(allocator_, {"世界"}, args_array);
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "你好 {0}", args_array, rendered));
  EXPECT_EQ("你好 世界", rendered);
}

TEST_F(TestObExprAIPrompt, json_argument_is_rendered_as_json)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args_array));
  ASSERT_EQ(OB_SUCCESS, append_json_arg(allocator_, args_array, "{\"a\":\"1\"}"));
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "value {0}", args_array, rendered));
  // The exact json text depends on the json writer, only the content is pinned.
  EXPECT_EQ(0, rendered.compare(0, 6, "value "));
  EXPECT_NE(std::string::npos, rendered.find("\"a\""));
  EXPECT_NE(std::string::npos, rendered.find("1"));
}

TEST_F(TestObExprAIPrompt, nested_prompt_object_is_rendered_recursively)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args_array));
  ASSERT_EQ(OB_SUCCESS, append_json_arg(
      allocator_, args_array, "{\"template\":\"inner {0}\",\"args\":[\"z\"]}"));
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "outer {0}", args_array, rendered));
  EXPECT_EQ("outer inner z", rendered);
}

TEST_F(TestObExprAIPrompt, placeholder_index_out_of_range_is_rejected)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  build_str_args(allocator_, {"x"}, args_array);
  EXPECT_NE(OB_SUCCESS, render(allocator_, "value {3}", args_array, rendered));
}

TEST_F(TestObExprAIPrompt, no_placeholder_keeps_template)
{
  ObJsonArray *args_array = nullptr;
  std::string rendered;
  build_str_args(allocator_, {"x"}, args_array);
  ASSERT_EQ(OB_SUCCESS, render(allocator_, "plain text", args_array, rendered));
  EXPECT_EQ("plain text", rendered);
}

TEST_F(TestObExprAIPrompt, endpoint_params_dimensions_and_batch_size)
{
  ObAIFuncUtils::ObAIEndpointParams params;
  ObString text = make_string("{\"dimensions\": 8, \"batch_size\": 4}");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::parse_endpoint_params(allocator_, text, params));
  EXPECT_EQ(8, params.dimensions_);
  EXPECT_EQ(4, params.batch_size_);
}

TEST_F(TestObExprAIPrompt, endpoint_params_empty)
{
  ObAIFuncUtils::ObAIEndpointParams params;
  ObString text = make_string("");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::parse_endpoint_params(allocator_, text, params));
  EXPECT_EQ(0, params.dimensions_);
  EXPECT_EQ(0, params.batch_size_);
}

TEST_F(TestObExprAIPrompt, endpoint_params_ignore_unknown_member)
{
  ObAIFuncUtils::ObAIEndpointParams params;
  ObString text = make_string("{\"other\": 3}");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::parse_endpoint_params(allocator_, text, params));
  EXPECT_EQ(0, params.dimensions_);
  EXPECT_EQ(0, params.batch_size_);
}

} // namespace sql
} // namespace oceanbase
