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

// Unit tests for the "file as a database" scan layer:
//   CSV schema inference / read options, JSONL field union, file change
//   detection, glob, COPY field escaping and Parquet decoding.
//
// The Parquet cases read the fixtures shipped with the mysql_test suite
// (tools/deploy/mysql_test/test_suite/file_sql/data).  Set
// FILE_SQL_TEST_DATA_DIR to point at that directory; when it is missing the
// Parquet cases are skipped so the rest of the suite still runs.

#include <gtest/gtest.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "sql/engine/basic/ob_file_scan_utils.h"

using namespace oceanbase::sql::file_sql; // NOLINT

namespace
{

class TempDir
{
public:
  TempDir()
  {
    char tmpl[] = "/tmp/file_scan_ut_XXXXXX";
    char *dir = mkdtemp(tmpl);
    dir_ = (NULL == dir) ? "/tmp" : dir;
  }
  ~TempDir()
  {
    for (size_t i = 0; i < files_.size(); ++i) {
      unlink(files_[i].c_str());
    }
    if ("/tmp" != dir_) {
      rmdir(dir_.c_str());
    }
  }
  std::string write(const std::string &name, const std::string &content)
  {
    std::string path = dir_ + "/" + name;
    FILE *fp = fopen(path.c_str(), "wb");
    if (NULL != fp) {
      if (!content.empty()) {
        fwrite(content.data(), 1, content.size(), fp);
      }
      fclose(fp);
    }
    files_.push_back(path);
    return path;
  }
  const std::string &dir() const { return dir_; }

private:
  std::string dir_;
  std::vector<std::string> files_;
};

struct ScanResult
{
  int ret_ = FS_OK;
  std::vector<FileColumnSchema> schema_;
  std::vector<FileRow> rows_;
  std::string err_;
};

ScanResult scan(const FileReadOptions &options, const std::string &path, int64_t max_rows = -1)
{
  ScanResult result;
  FileScanReader *reader = NULL;
  result.ret_ = create_file_scan_reader(options, path, reader, result.err_);
  if (FS_OK != result.ret_) {
    return result;
  }
  if (NULL == reader) {
    result.ret_ = FS_ERR_INVALID_ARGUMENT;
    return result;
  }
  result.ret_ = reader->infer_schema(result.schema_, result.err_);
  if (FS_OK != result.ret_) {
    return result;
  }
  result.ret_ = reader->open(result.err_);
  if (FS_OK != result.ret_) {
    return result;
  }
  int64_t count = 0;
  while (FS_OK == result.ret_ && (max_rows < 0 || count < max_rows)) {
    FileRow row;
    bool eof = false;
    result.ret_ = reader->next(row, eof, result.err_);
    if (FS_OK != result.ret_) {
      break;
    } else if (eof) {
      break;
    } else {
      result.rows_.push_back(row);
      ++count;
    }
  }
  reader->close();
  delete reader;
  return result;
}

const FileColumnSchema *find_column(const std::vector<FileColumnSchema> &schema, const std::string &name)
{
  for (size_t i = 0; i < schema.size(); ++i) {
    if (schema[i].name_ == name) {
      return &schema[i];
    }
  }
  return NULL;
}

std::string test_data_dir()
{
  const char *env = getenv("FILE_SQL_TEST_DATA_DIR");
  if (NULL != env && '\0' != env[0]) {
    return std::string(env);
  }
  const char *candidates[] = {
    "tools/deploy/mysql_test/test_suite/file_sql/data",
    "../tools/deploy/mysql_test/test_suite/file_sql/data",
    "../../tools/deploy/mysql_test/test_suite/file_sql/data",
  };
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    std::string path = candidates[i];
    struct stat st;
    if (0 == stat((path + "/p_plain.parquet").c_str(), &st)) {
      return path;
    }
  }
  return std::string();
}

TEST(FileScanUtils, CsvSchemaInference)
{
  TempDir dir;
  std::string path = dir.write("people.csv",
                               "id,name,score\n"
                               "1,Alice,90.5\n"
                               "2,Bob,85\n");
  FileReadOptions options;
  options.format_ = "csv";
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(3u, result.schema_.size());
  EXPECT_EQ(FT_BIGINT, result.schema_[0].type_);
  EXPECT_EQ(FT_VARCHAR, result.schema_[1].type_);
  EXPECT_EQ(FT_DOUBLE, result.schema_[2].type_);
  ASSERT_EQ(2u, result.rows_.size());
  EXPECT_EQ(1, result.rows_[0][0].int_val_);
  EXPECT_EQ("Alice", result.rows_[0][1].str_val_);
  EXPECT_DOUBLE_EQ(90.5, result.rows_[0][2].double_val_);
}

TEST(FileScanUtils, CsvCrlfQuoteAndEmbeddedNewline)
{
  TempDir dir;
  std::string path = dir.write("quoted.csv",
                               "id,name,note\r\n"
                               "1,\"Alice, A\",\"line1\nline2\"\r\n"
                               "2,\"quote \"\"ok\"\"\",plain\r\n");
  FileReadOptions options;
  options.format_ = "csv";
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(2u, result.rows_.size());
  EXPECT_EQ("Alice, A", result.rows_[0][1].str_val_);
  EXPECT_EQ("line1\nline2", result.rows_[0][2].str_val_);
  EXPECT_EQ("quote \"ok\"", result.rows_[1][1].str_val_);
}

TEST(FileScanUtils, CsvNullAndCustomNullString)
{
  TempDir dir;
  std::string path = dir.write("nulls.csv",
                               "id,val\n"
                               "1,NA\n"
                               "2,5\n"
                               "3,\n");
  FileReadOptions options;
  options.format_ = "csv";
  options.null_str_set_ = true;
  options.null_str_ = "NA";
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(3u, result.rows_.size());
  // null='NA' must not force the column to VARCHAR: the remaining value is int.
  ASSERT_EQ(FT_BIGINT, result.schema_[1].type_);
  EXPECT_TRUE(result.rows_[0][1].is_null());
  EXPECT_EQ(5, result.rows_[1][1].int_val_);
  // Unquoted empty fields stay NULL, a quoted empty field would be ''.
  EXPECT_TRUE(result.rows_[2][1].is_null());
}

TEST(FileScanUtils, CsvTypeWidening)
{
  TempDir dir;
  std::string path = dir.write("wide.csv",
                               "a,b,c\n"
                               "1,true,2020-02-29\n"
                               "2.5,false,2020-03-01\n");
  FileReadOptions options;
  options.format_ = "csv";
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(3u, result.schema_.size());
  EXPECT_EQ(FT_DOUBLE, result.schema_[0].type_);
  EXPECT_EQ(FT_BOOLEAN, result.schema_[1].type_);
  EXPECT_EQ(FT_DATE, result.schema_[2].type_);
}

TEST(FileScanUtils, CsvDateAndDatetimeInference)
{
  TempDir dir;
  std::string path = dir.write("time.csv",
                               "d,dt,bad\n"
                               "2020-02-29,2020-02-29 23:59:59,2019-02-29\n"
                               "1970-01-01,1970-01-01 00:00:00,2020-01-01\n");
  FileReadOptions options;
  options.format_ = "csv";
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(3u, result.schema_.size());
  EXPECT_EQ(FT_DATE, result.schema_[0].type_);
  EXPECT_EQ(FT_DATETIME, result.schema_[1].type_);
  // 非法日期不能被推断成 DATE
  EXPECT_EQ(FT_VARCHAR, result.schema_[2].type_);

  int32_t days = 0;
  ASSERT_EQ(FS_OK, parse_date_text("2020-02-29", days));
  EXPECT_EQ("2020-02-29", format_date_days(days));
  int64_t us = 0;
  ASSERT_EQ(FS_OK, parse_datetime_text("2020-02-29 12:00:00.123456", us));
  EXPECT_EQ("2020-02-29 12:00:00.123456", format_datetime_us(us));
}

TEST(FileScanUtils, CsvAllNullColumn)
{
  TempDir dir;
  std::string path = dir.write("allnull.csv",
                               "a,b\n"
                               "1,\n"
                               "2,\n");
  FileReadOptions options;
  options.format_ = "csv";
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(2u, result.schema_.size());
  EXPECT_EQ(FT_BIGINT, result.schema_[0].type_);
  EXPECT_EQ(FT_NULL, result.schema_[1].type_);
}

TEST(FileScanUtils, CsvWithoutHeaderAndCustomDelimiter)
{
  TempDir dir;
  std::string path = dir.write("pipe.csv",
                               "1|Alice\n"
                               "2|Bob\n");
  FileReadOptions options;
  options.format_ = "csv";
  options.delimiter_ = '|';
  options.has_header_ = false;
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(2u, result.schema_.size());
  ASSERT_EQ(2u, result.rows_.size());
  EXPECT_EQ(1, result.rows_[0][0].int_val_);
  EXPECT_EQ("Alice", result.rows_[0][1].str_val_);
}

TEST(FileScanUtils, CsvBackslashEscapeOption)
{
  TempDir dir;
  std::string path = dir.write("escaped.csv",
                               "id,val\n"
                               "1,\"a\\\"b\"\n");
  FileReadOptions options;
  options.format_ = "csv";
  options.escape_ = '\\';
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(1u, result.rows_.size());
  EXPECT_EQ("a\"b", result.rows_[0][1].str_val_);
}

TEST(FileScanUtils, CsvBadRowDiagnostic)
{
  TempDir dir;
  std::string path = dir.write("bad.csv",
                               "id,val\n"
                               "1,\"unterminated\n");
  FileReadOptions options;
  options.format_ = "csv";
  ScanResult result = scan(options, path);
  EXPECT_NE(FS_OK, result.ret_);
  EXPECT_FALSE(result.err_.empty());
}

TEST(FileScanUtils, CsvStreamingBatches)
{
  TempDir dir;
  std::string content = "id\n";
  for (int i = 0; i < 5000; ++i) {
    content += std::to_string(i) + "\n";
  }
  std::string path = dir.write("big.csv", content);
  FileReadOptions options;
  options.format_ = "csv";
  FileScanReader *reader = NULL;
  std::string err;
  ASSERT_EQ(FS_OK, create_file_scan_reader(options, path, reader, err));
  ASSERT_TRUE(NULL != reader);
  std::vector<FileColumnSchema> schema;
  ASSERT_EQ(FS_OK, reader->infer_schema(schema, err));
  ASSERT_EQ(1u, schema.size());
  EXPECT_EQ(FT_BIGINT, schema[0].type_);
  ASSERT_EQ(FS_OK, reader->open(err));
  int64_t count = 0;
  bool eof = false;
  while (!eof) {
    FileRow row;
    ASSERT_EQ(FS_OK, reader->next(row, eof, err)) << err;
    if (!eof) {
      ++count;
    }
  }
  reader->close();
  delete reader;
  EXPECT_EQ(5000, count);
}

TEST(FileScanUtils, JsonlFieldUnionAndMissingValues)
{
  TempDir dir;
  std::string path = dir.write("union.jsonl",
                               "{\"id\":1,\"name\":\"Alice\"}\n"
                               "{\"id\":2,\"score\":90.5}\n"
                               "{\"id\":3}\n");
  FileReadOptions options;
  options.format_ = "jsonl";
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  ASSERT_EQ(3u, result.schema_.size());
  EXPECT_TRUE(NULL != find_column(result.schema_, "id"));
  EXPECT_TRUE(NULL != find_column(result.schema_, "name"));
  EXPECT_TRUE(NULL != find_column(result.schema_, "score"));
  ASSERT_EQ(3u, result.rows_.size());
  EXPECT_TRUE(result.rows_[1][1].is_null());
  EXPECT_DOUBLE_EQ(90.5, result.rows_[1][2].double_val_);
  EXPECT_TRUE(result.rows_[2][1].is_null());
  EXPECT_TRUE(result.rows_[2][2].is_null());
}

TEST(FileScanUtils, JsonlTypeConflictFallsBackToVarchar)
{
  TempDir dir;
  std::string path = dir.write("conflict.jsonl",
                               "{\"id\":1,\"v\":2}\n"
                               "{\"id\":2,\"v\":\"x\"}\n");
  FileReadOptions options;
  options.format_ = "jsonl";
  ScanResult result = scan(options, path);
  ASSERT_EQ(FS_OK, result.ret_) << result.err_;
  const FileColumnSchema *v = find_column(result.schema_, "v");
  ASSERT_TRUE(NULL != v);
  EXPECT_EQ(FT_VARCHAR, v->type_);
}

TEST(FileScanUtils, JsonlRejectsNestedAndBadLines)
{
  TempDir dir;
  std::string nested = dir.write("nested.jsonl", "{\"id\":1,\"obj\":{\"a\":1}}\n");
  std::string bad = dir.write("bad.jsonl", "{\"id\":1}\nnot-a-json\n");
  FileReadOptions options;
  options.format_ = "jsonl";
  ScanResult nested_result = scan(options, nested);
  EXPECT_EQ(FS_ERR_UNSUPPORTED, nested_result.ret_);
  FileReadOptions options2;
  options2.format_ = "jsonl";
  ScanResult bad_result = scan(options2, bad);
  EXPECT_EQ(FS_ERR_BAD_DATA, bad_result.ret_);
  EXPECT_FALSE(bad_result.err_.empty());
}

TEST(FileScanUtils, GlobDiscoversRegularFilesOnly)
{
  TempDir dir;
  dir.write("a.csv", "id\n1\n");
  dir.write("b.csv", "id\n2\n");
  dir.write("c.jsonl", "{\"id\":3}\n");
  std::vector<std::string> files;
  std::string err;
  ASSERT_EQ(FS_OK, glob_files(dir.dir() + "/*.csv", files, err)) << err;
  ASSERT_EQ(2u, files.size());
  EXPECT_TRUE(files[0].find("a.csv") != std::string::npos);
  EXPECT_TRUE(files[1].find("b.csv") != std::string::npos);
}

TEST(FileScanUtils, FileSignatureDetectsChange)
{
  TempDir dir;
  std::string path = dir.write("sig.csv", "id\n1\n");
  FileSignature before;
  std::string err;
  ASSERT_EQ(FS_OK, stat_file_signature(path, before, err)) << err;
  EXPECT_TRUE(is_regular_file(path));
  {
    FILE *fp = fopen(path.c_str(), "ab");
    ASSERT_TRUE(NULL != fp);
    fputs("2\n", fp);
    fclose(fp);
  }
  FileSignature after;
  ASSERT_EQ(FS_OK, stat_file_signature(path, after, err)) << err;
  EXPECT_TRUE(before != after);
}

TEST(FileScanUtils, CsvEscapeFieldForCopy)
{
  EXPECT_EQ("plain", csv_escape_field("plain", ',', '"', '"'));
  EXPECT_EQ("\"a,b\"", csv_escape_field("a,b", ',', '"', '"'));
  EXPECT_EQ("\"a\"\"b\"", csv_escape_field("a\"b", ',', '"', '"'));
  EXPECT_EQ("\"a\nb\"", csv_escape_field("a\nb", ',', '"', '"'));
  // 反斜杠转义风格（COPY ... ESCAPE '\'）
  EXPECT_EQ("\"a\\\"b\"", csv_escape_field("a\"b", ',', '"', '\\'));
}

TEST(FileScanUtils, PathNormalization)
{
  TempDir dir;
  ASSERT_EQ(0, mkdir((dir.dir() + "/a").c_str(), 0755));
  ASSERT_EQ(0, mkdir((dir.dir() + "/b").c_str(), 0755));
  std::string file = dir.write("b/c.csv", "x\n1\n");
  std::string normalized;
  std::string err;
  ASSERT_EQ(FS_OK, normalize_path(dir.dir() + "/a/../b/./c.csv", normalized, err)) << err;
  EXPECT_EQ(file, normalized);
}

TEST(FileScanUtils, ParquetPlainSnappyDictionary)
{
  const std::string data_dir = test_data_dir();
  if (data_dir.empty()) {
    std::cout << "[  SKIPPED ] Parquet fixtures are not available; set "
                 "FILE_SQL_TEST_DATA_DIR to run them" << std::endl;
  }
  const char *files[] = {"p_plain.parquet", "p_snappy.parquet", "p_dict.parquet"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    FileReadOptions options;
    options.format_ = "parquet";
    ScanResult result = scan(options, data_dir + "/" + files[i]);
    ASSERT_EQ(FS_OK, result.ret_) << files[i] << ": " << result.err_;
    ASSERT_EQ(6u, result.schema_.size()) << files[i];
    EXPECT_EQ("id", result.schema_[0].name_);
    EXPECT_EQ("name", result.schema_[1].name_);
    EXPECT_EQ("score", result.schema_[2].name_);
    EXPECT_EQ("dept", result.schema_[3].name_);
    EXPECT_EQ("birth", result.schema_[4].name_);
    EXPECT_EQ("active", result.schema_[5].name_);
    EXPECT_EQ(FT_BIGINT, result.schema_[0].type_) << files[i];
    EXPECT_EQ(FT_VARCHAR, result.schema_[1].type_) << files[i];
    EXPECT_EQ(FT_DOUBLE, result.schema_[2].type_) << files[i];
    EXPECT_EQ(FT_VARCHAR, result.schema_[3].type_) << files[i];
    EXPECT_EQ(FT_DATE, result.schema_[4].type_) << files[i];
    EXPECT_EQ(FT_BOOLEAN, result.schema_[5].type_) << files[i];
    ASSERT_EQ(10u, result.rows_.size()) << files[i];
    for (size_t r = 0; r < result.rows_.size(); ++r) {
      EXPECT_EQ(static_cast<int64_t>(r) + 1, result.rows_[r][0].int_val_) << files[i];
    }
    // Values exercise dictionary pages, UTF-8, decimals and NULL columns.
    EXPECT_EQ("n01", result.rows_[0][1].str_val_);
    EXPECT_DOUBLE_EQ(1.5, result.rows_[0][2].double_val_);
    EXPECT_EQ("eng", result.rows_[0][3].str_val_);
    EXPECT_EQ("张三", result.rows_[2][1].str_val_);
    EXPECT_TRUE(result.rows_[2][2].is_null());
    EXPECT_TRUE(result.rows_[3][3].is_null());
    EXPECT_EQ("a,b", result.rows_[4][1].str_val_);
    EXPECT_TRUE(result.rows_[4][5].is_null());
  }
}

} // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
