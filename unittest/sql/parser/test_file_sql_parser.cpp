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

// Parser-level tests for the file SQL syntax:
//   read_csv / read_json / read_ndjson / read_parquet / glob, named and
//   positional arguments, table aliases, DESCRIBE and COPY ... TO.

#include <gtest/gtest.h>

#include <memory>
#include <new>

#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

class FileSqlParserTest : public ::testing::Test
{
protected:
  void SetUp() override { allocator_.reset(); }
  void TearDown() override
  {
    if (NULL != parse_result_.result_tree_) {
      parser_->free_result(parse_result_);
    }
    parser_.reset();
    allocator_.reset();
  }

  void parse(const char *sql)
  {
    if (parser_.get() != NULL) {
      if (NULL != parse_result_.result_tree_) {
        parser_->free_result(parse_result_);
      }
      parser_.reset();
      memset(&parse_result_, 0, sizeof(parse_result_));
    }
    parser_.reset(new (std::nothrow) ObParser(allocator_, DEFAULT_MYSQL_MODE));
    ASSERT_TRUE(parser_.get() != NULL);
    ASSERT_EQ(OB_SUCCESS, parser_->parse(ObString::make_string(sql), parse_result_)) << sql;
    ASSERT_TRUE(NULL != parse_result_.result_tree_) << sql;
  }

  const ParseNode *stmt() const
  {
    const ParseNode *root = parse_result_.result_tree_;
    if (NULL != root && T_STMT_LIST == root->type_ && root->num_child_ > 0) {
      root = root->children_[0];
    }
    return root;
  }

  ObArenaAllocator allocator_;
  std::unique_ptr<ObParser> parser_;
  ParseResult parse_result_;
};

TEST_F(FileSqlParserTest, ReadCsvPositionalAndNamedArgs)
{
  parse("SELECT * FROM read_csv('/tmp/people.csv', header = true, null = 'NA', "
        "delimiter = ',', quote = '\"') t");
  EXPECT_EQ(T_SELECT, stmt()->type_);
}

TEST_F(FileSqlParserTest, ReadCsvAliasForms)
{
  parse("SELECT t.id FROM read_csv('/tmp/people.csv') AS t");
  EXPECT_EQ(T_SELECT, stmt()->type_);
  ASSERT_TRUE(NULL != stmt()->children_[PARSE_SELECT_FROM]);
}

TEST_F(FileSqlParserTest, ReadJsonAndNdjson)
{
  parse("SELECT * FROM read_json('/tmp/people.jsonl')");
  EXPECT_EQ(T_SELECT, stmt()->type_);
  parse("SELECT * FROM read_ndjson('/tmp/people.jsonl', format = 'ndjson')");
  EXPECT_EQ(T_SELECT, stmt()->type_);
}

TEST_F(FileSqlParserTest, ReadParquetAndGlob)
{
  parse("SELECT * FROM read_parquet('/tmp/people.parquet')");
  EXPECT_EQ(T_SELECT, stmt()->type_);
  parse("SELECT * FROM glob('/tmp/*.csv')");
  EXPECT_EQ(T_SELECT, stmt()->type_);
}

TEST_F(FileSqlParserTest, DescribeFileTable)
{
  parse("DESCRIBE SELECT * FROM read_csv('/tmp/people.csv', header = true)");
  EXPECT_EQ(T_EXPLAIN, stmt()->type_);
}

TEST_F(FileSqlParserTest, CopyTableTo)
{
  parse("COPY t TO '/tmp/out.csv' WITH (FORMAT CSV, DELIMITER ',', HEADER true)");
  EXPECT_EQ(T_SELECT, stmt()->type_);
}

TEST_F(FileSqlParserTest, CopyQueryTo)
{
  parse("COPY (SELECT id, name FROM t ORDER BY id) TO '/tmp/out.csv' (FORMAT CSV)");
  EXPECT_EQ(T_SELECT, stmt()->type_);
}

TEST_F(FileSqlParserTest, CopyFileTableTo)
{
  parse("COPY read_csv('/tmp/in.csv') TO '/tmp/out.csv' (FORMAT CSV)");
  EXPECT_EQ(T_SELECT, stmt()->type_);
}

TEST_F(FileSqlParserTest, RejectsUnclosedParen)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, DEFAULT_MYSQL_MODE);
  ParseResult result;
  EXPECT_NE(OB_SUCCESS, parser.parse(ObString::make_string(
      "SELECT * FROM read_csv('/tmp/people.csv'"), result));
  parser.free_result(result);
}

} // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
