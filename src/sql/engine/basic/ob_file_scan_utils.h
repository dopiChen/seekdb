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

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

/**
 * Logical column types supported by read_csv / read_json / read_parquet.
 * The order is also the widening order used by type inference.
 */
enum FileColumnType : int32_t
{
  FT_NULL = 0,
  FT_BOOLEAN = 1,
  FT_BIGINT = 2,
  FT_DOUBLE = 3,
  FT_DATE = 4,
  FT_DATETIME = 5,
  FT_VARCHAR = 6,
};

enum FileScanRet : int32_t
{
  FS_OK = 0,
  FS_ERR_IO = 1,
  FS_ERR_BAD_DATA = 2,
  FS_ERR_UNSUPPORTED = 3,
  FS_ERR_FILE_CHANGED = 4,
  FS_ERR_INVALID_ARGUMENT = 5,
  FS_ERR_NOT_EXIST = 6,
};

const char *file_column_type_name(FileColumnType type);

// Map a FileScanRet code to a client visible oceanbase error code, so that the
// SQL layer never leaks the internal FileScanRet values (e.g. 3) to clients.
int file_scan_ret_to_ob_error(int fs_ret);
bool file_column_type_from_name(const std::string &name, FileColumnType &type);

struct FileSignature
{
  int64_t size_ = 0;
  int64_t mtime_sec_ = 0;
  int64_t mtime_nsec_ = 0;
  int64_t inode_ = 0;
  bool operator==(const FileSignature &other) const
  {
    return size_ == other.size_ && mtime_sec_ == other.mtime_sec_ &&
           mtime_nsec_ == other.mtime_nsec_ && inode_ == other.inode_;
  }
  bool operator!=(const FileSignature &other) const { return !(*this == other); }
};

struct FileColumnSchema
{
  std::string name_;
  FileColumnType type_ = FT_NULL;
  int64_t varchar_length_ = 0;
  // fractional-seconds scale for DATE/DATETIME columns, -1 means use the default
  int64_t scale_ = -1;
  bool nullable_ = true;
  // parquet physical description, only used by the parquet reader
  int32_t parquet_type_ = -1;
  int32_t parquet_converted_type_ = -1;
  void reset() { *this = FileColumnSchema(); }
};

struct FileCell
{
  FileColumnType type_ = FT_NULL;
  bool bool_val_ = false;
  int64_t int_val_ = 0;
  double double_val_ = 0.0;
  int32_t date_days_ = 0;
  int64_t datetime_us_ = 0;
  std::string str_val_;
  bool is_null() const { return FT_NULL == type_; }
  void reset()
  {
    type_ = FT_NULL;
    bool_val_ = false;
    int_val_ = 0;
    double_val_ = 0.0;
    date_days_ = 0;
    datetime_us_ = 0;
    str_val_.clear();
  }
};

typedef std::vector<FileCell> FileRow;

struct FileReadOptions
{
  std::string format_;                 // csv / jsonl / parquet / glob
  char delimiter_ = ',';
  char quote_ = '"';
  char escape_ = '"';
  bool has_header_ = true;
  bool null_str_set_ = false;
  std::string null_str_;
  int64_t sample_size_ = -1;           // only -1 (full file inference) is supported
  int64_t max_line_size_ = 64LL << 20; // 64MB per record
  void reset() { *this = FileReadOptions(); }
};

/**
 * Streaming reader interface. A reader must never materialize the whole file:
 *  - infer_schema() scans the whole file once (schema inference is defined as full scan)
 *  - open()/next()/close() provide streaming row access
 */
class FileScanReader
{
public:
  FileScanReader() {}
  virtual ~FileScanReader() {}
  void set_path(const std::string &path) { path_ = path; }
  const std::string &get_path() const { return path_; }
  virtual int infer_schema(std::vector<FileColumnSchema> &schema, std::string &err) = 0;
  virtual int open(std::string &err) = 0;
  virtual int next(FileRow &row, bool &eof, std::string &err) = 0;
  virtual void close() = 0;
  virtual int64_t estimated_rows() const { return -1; }
  virtual bool is_glob() const { return false; }
protected:
  std::string path_;
};

/**
 * Create a reader for the given format.
 */
int create_file_scan_reader(const FileReadOptions &options,
                            const std::string &path,
                            FileScanReader *&reader,
                            std::string &err);

/**
 * utility helpers
 */
int stat_file_signature(const std::string &path, FileSignature &sig, std::string &err);
bool is_regular_file(const std::string &path);
int normalize_path(const std::string &path, std::string &normalized, std::string &err);
int glob_files(const std::string &pattern,
               std::vector<std::string> &files,
               std::string &err);
// infer column type of a single text value, used by csv
FileColumnType infer_text_type(const std::string &text, int64_t &varchar_length);
FileColumnType merge_column_type(FileColumnType left, FileColumnType right);
int parse_text_to_cell(const std::string &text, FileColumnType type, FileCell &cell);
int parse_date_text(const std::string &text, int32_t &days);
int parse_datetime_text(const std::string &text, int64_t &us);
int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d);
void civil_from_days(int32_t days, int32_t &y, int32_t &m, int32_t &d);
std::string format_date_days(int32_t days);
std::string format_datetime_us(int64_t us);
// quote one csv field for export (used by COPY)
std::string csv_escape_field(const std::string &field, char delimiter, char quote, char escape);

int create_parquet_reader(const FileReadOptions &options,
                          const std::string &path,
                          FileScanReader *&reader,
                          std::string &err);

} // namespace file_sql
} // namespace sql
} // namespace oceanbase

