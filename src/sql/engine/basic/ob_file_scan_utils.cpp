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

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

int file_scan_ret_to_ob_error(int fs_ret)
{
  int ret = OB_SUCCESS;
  switch (fs_ret) {
    case FS_OK: ret = OB_SUCCESS; break;
    case FS_ERR_IO: ret = OB_IO_ERROR; break;
    case FS_ERR_BAD_DATA: ret = OB_INVALID_DATA; break;
    case FS_ERR_UNSUPPORTED: ret = OB_NOT_SUPPORTED; break;
    case FS_ERR_FILE_CHANGED: ret = OB_INVALID_DATA; break;
    case FS_ERR_INVALID_ARGUMENT: ret = OB_INVALID_ARGUMENT; break;
    case FS_ERR_NOT_EXIST: ret = OB_FILE_NOT_EXIST; break;
    default: ret = OB_ERR_UNEXPECTED; break;
  }
  return ret;
}

const char *file_column_type_name(FileColumnType type)
{
  const char *name = "varchar";
  switch (type) {
    case FT_NULL: name = "null"; break;
    case FT_BOOLEAN: name = "boolean"; break;
    case FT_BIGINT: name = "bigint"; break;
    case FT_DOUBLE: name = "double"; break;
    case FT_DATE: name = "date"; break;
    case FT_DATETIME: name = "datetime"; break;
    case FT_VARCHAR: name = "varchar"; break;
    default: name = "varchar"; break;
  }
  return name;
}

bool file_column_type_from_name(const std::string &name, FileColumnType &type)
{
  bool found = true;
  std::string lower;
  lower.reserve(name.size());
  for (size_t i = 0; i < name.size(); ++i) {
    lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(name[i]))));
  }
  if ("null" == lower) {
    type = FT_NULL;
  } else if ("boolean" == lower || "bool" == lower) {
    type = FT_BOOLEAN;
  } else if ("bigint" == lower || "int" == lower || "integer" == lower || "int64" == lower) {
    type = FT_BIGINT;
  } else if ("double" == lower || "real" == lower || "float" == lower) {
    type = FT_DOUBLE;
  } else if ("date" == lower) {
    type = FT_DATE;
  } else if ("datetime" == lower || "timestamp" == lower) {
    type = FT_DATETIME;
  } else if ("varchar" == lower || "string" == lower || "text" == lower) {
    type = FT_VARCHAR;
  } else {
    found = false;
  }
  return found;
}

namespace
{

inline int64_t floor_div(int64_t a, int64_t b)
{
  int64_t q = a / b;
  int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) {
    --q;
  }
  return q;
}

struct FileCloser
{
  explicit FileCloser(FILE *fp) : fp_(fp) {}
  ~FileCloser() { if (NULL != fp_) { fclose(fp_); } }
  FILE *fp_;
};

void set_err(std::string &err, const char *fmt, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  err.assign(buf);
}

bool is_space(char c) { return ' ' == c || '\t' == c || '\n' == c || '\r' == c; }

bool parse_int64_strict(const std::string &text, int64_t &value)
{
  bool ok = false;
  if (!text.empty()) {
    errno = 0;
    char *end = NULL;
    const long long v = strtoll(text.c_str(), &end, 10);
    if (ERANGE != errno && NULL != end && '\0' == *end && end != text.c_str()) {
      // reject leading/trailing spaces, only allow an optional sign
      bool digits_only = true;
      for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (0 == i && ('-' == c || '+' == c)) {
          continue;
        }
        if (c < '0' || c > '9') {
          digits_only = false;
          break;
        }
      }
      if (digits_only) {
        value = static_cast<int64_t>(v);
        ok = true;
      }
    }
  }
  return ok;
}

bool parse_double_strict(const std::string &text, double &value)
{
  bool ok = false;
  if (!text.empty()) {
    errno = 0;
    char *end = NULL;
    const double v = strtod(text.c_str(), &end);
    if (0 == errno && NULL != end && '\0' == *end && end != text.c_str()) {
      // reject nan/inf and surrounding spaces
      bool clean = true;
      for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' ||
              c == 'e' || c == 'E')) {
          clean = false;
          break;
        }
      }
      if (clean) {
        value = v;
        ok = true;
      }
    }
  }
  return ok;
}

bool is_leap_year(int64_t y)
{
  return (0 == y % 4 && 0 != y % 100) || 0 == y % 400;
}

int days_in_month(int64_t y, int64_t m)
{
  static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days = 0;
  if (m >= 1 && m <= 12) {
    days = mdays[m - 1];
    if (2 == m && is_leap_year(y)) {
      days = 29;
    }
  }
  return days;
}

int parse_digits(const std::string &text, size_t &pos, int digits, int64_t &value)
{
  int ret = FS_OK;
  value = 0;
  if (pos + static_cast<size_t>(digits) > text.size()) {
    ret = FS_ERR_BAD_DATA;
  } else {
    for (int i = 0; FS_OK == ret && i < digits; ++i) {
      const char c = text[pos + i];
      if (c < '0' || c > '9') {
        ret = FS_ERR_BAD_DATA;
      } else {
        value = value * 10 + (c - '0');
      }
    }
    pos += digits;
  }
  return ret;
}

} // anonymous namespace

int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d)
{
  // Howard Hinnant's civil_from_days/days_from_civil: January and February
  // are treated as months 13 and 14 of the previous year.
  if (m <= 2) {
    --y;
  }
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int32_t days, int32_t &y, int32_t &m, int32_t &d)
{
  int64_t z = static_cast<int64_t>(days) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  d = static_cast<int32_t>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int32_t>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int32_t>(yy + (m <= 2 ? 1 : 0));
}

int parse_date_text(const std::string &text, int32_t &days)
{
  int ret = FS_OK;
  size_t pos = 0;
  int64_t y = 0;
  int64_t m = 0;
  int64_t d = 0;
  if (10 != text.size()) {
    ret = FS_ERR_BAD_DATA;
  } else if (FS_OK != parse_digits(text, pos, 4, y) ||
             pos >= text.size() || '-' != text[pos++]) {
    ret = FS_ERR_BAD_DATA;
  } else if (FS_OK != parse_digits(text, pos, 2, m) ||
             pos >= text.size() || '-' != text[pos++]) {
    ret = FS_ERR_BAD_DATA;
  } else if (FS_OK != parse_digits(text, pos, 2, d)) {
    ret = FS_ERR_BAD_DATA;
  } else if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) {
    ret = FS_ERR_BAD_DATA;
  } else {
    days = static_cast<int32_t>(days_from_civil(y, static_cast<uint32_t>(m),
                                                static_cast<uint32_t>(d)));
  }
  return ret;
}

int parse_datetime_text(const std::string &text, int64_t &us)
{
  int ret = FS_OK;
  int32_t days = 0;
  int64_t hour = 0;
  int64_t minute = 0;
  int64_t second = 0;
  int64_t fraction = 0;
  int64_t fraction_digits = 0;
  if (text.size() < 19 || (' ' != text[10] && 'T' != text[10])) {
    ret = FS_ERR_BAD_DATA;
  } else if (FS_OK != parse_date_text(text.substr(0, 10), days)) {
    ret = FS_ERR_BAD_DATA;
  } else {
    size_t pos = 11;
    if (FS_OK != parse_digits(text, pos, 2, hour) ||
        pos >= text.size() || ':' != text[pos++]) {
      ret = FS_ERR_BAD_DATA;
    } else if (FS_OK != parse_digits(text, pos, 2, minute) ||
               pos >= text.size() || ':' != text[pos++]) {
      ret = FS_ERR_BAD_DATA;
    } else if (FS_OK != parse_digits(text, pos, 2, second)) {
      ret = FS_ERR_BAD_DATA;
    } else if (pos < text.size() && '.' == text[pos]) {
      ++pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        if (fraction_digits < 6) {
          fraction = fraction * 10 + (text[pos] - '0');
          ++fraction_digits;
        }
        ++pos;
      }
      while (fraction_digits < 6) {
        fraction *= 10;
        ++fraction_digits;
      }
    }
    if (FS_OK == ret &&
        (pos != text.size() || hour > 23 || minute > 59 || second > 59 ||
         0 == fraction_digits && fraction != 0)) {
      ret = FS_ERR_BAD_DATA;
    }
    if (FS_OK == ret) {
      us = (static_cast<int64_t>(days) * 86400 + hour * 3600 + minute * 60 + second) *
               1000000LL + fraction;
    }
  }
  return ret;
}

std::string format_date_days(int32_t days)
{
  int32_t y = 0;
  int32_t m = 0;
  int32_t d = 0;
  char buf[32];
  civil_from_days(days, y, m, d);
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
  return std::string(buf);
}

std::string format_datetime_us(int64_t us)
{
  const int64_t secs = floor_div(us, 1000000LL);
  int64_t frac = us - secs * 1000000LL;
  int32_t days = static_cast<int32_t>(floor_div(secs, 86400));
  int64_t sod = secs - static_cast<int64_t>(days) * 86400;
  char buf[64];
  char frac_buf[16];
  frac_buf[0] = '\0';
  if (frac > 0) {
    snprintf(frac_buf, sizeof(frac_buf), ".%06lld", static_cast<long long>(frac));
  }
  snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld%s",
           format_date_days(days).c_str(),
           static_cast<long long>(sod / 3600),
           static_cast<long long>((sod / 60) % 60),
           static_cast<long long>(sod % 60),
           frac_buf);
  return std::string(buf);
}

FileColumnType infer_text_type(const std::string &text, int64_t &varchar_length)
{
  FileColumnType type = FT_VARCHAR;
  int64_t int_val = 0;
  double double_val = 0.0;
  int32_t days = 0;
  int64_t us = 0;
  std::string lower;
  lower.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(text[i]))));
  }
  if (text.empty()) {
    type = FT_NULL;
  } else if ("true" == lower || "false" == lower) {
    type = FT_BOOLEAN;
  } else if (parse_int64_strict(text, int_val)) {
    type = FT_BIGINT;
  } else if (parse_double_strict(text, double_val)) {
    type = FT_DOUBLE;
  } else if (FS_OK == parse_date_text(text, days)) {
    type = FT_DATE;
  } else if (FS_OK == parse_datetime_text(text, us)) {
    type = FT_DATETIME;
  } else {
    type = FT_VARCHAR;
  }
  if (static_cast<int64_t>(text.size()) > varchar_length) {
    varchar_length = static_cast<int64_t>(text.size());
  }
  return type;
}

FileColumnType merge_column_type(FileColumnType left, FileColumnType right)
{
  FileColumnType type = FT_VARCHAR;
  if (FT_NULL == left) {
    type = right;
  } else if (FT_NULL == right) {
    type = left;
  } else if (left == right) {
    type = left;
  } else if ((FT_BIGINT == left && FT_DOUBLE == right) ||
             (FT_DOUBLE == left && FT_BIGINT == right)) {
    type = FT_DOUBLE;
  } else if ((FT_DATE == left && FT_DATETIME == right) ||
             (FT_DATETIME == left && FT_DATE == right)) {
    type = FT_DATETIME;
  } else {
    type = FT_VARCHAR;
  }
  return type;
}

int parse_text_to_cell(const std::string &text, FileColumnType type, FileCell &cell)
{
  int ret = FS_OK;
  cell.reset();
  switch (type) {
    case FT_NULL: {
      cell.type_ = FT_NULL;
      break;
    }
    case FT_BOOLEAN: {
      std::string lower;
      lower.reserve(text.size());
      for (size_t i = 0; i < text.size(); ++i) {
        lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(text[i]))));
      }
      if ("true" == lower || "1" == lower) {
        cell.type_ = FT_BOOLEAN;
        cell.bool_val_ = true;
      } else if ("false" == lower || "0" == lower) {
        cell.type_ = FT_BOOLEAN;
        cell.bool_val_ = false;
      } else {
        ret = FS_ERR_BAD_DATA;
      }
      break;
    }
    case FT_BIGINT: {
      int64_t value = 0;
      if (parse_int64_strict(text, value)) {
        cell.type_ = FT_BIGINT;
        cell.int_val_ = value;
      } else {
        ret = FS_ERR_BAD_DATA;
      }
      break;
    }
    case FT_DOUBLE: {
      double value = 0.0;
      int64_t int_value = 0;
      if (parse_double_strict(text, value)) {
        cell.type_ = FT_DOUBLE;
        cell.double_val_ = value;
      } else if (parse_int64_strict(text, int_value)) {
        cell.type_ = FT_DOUBLE;
        cell.double_val_ = static_cast<double>(int_value);
      } else {
        ret = FS_ERR_BAD_DATA;
      }
      break;
    }
    case FT_DATE: {
      int32_t days = 0;
      if (FS_OK == parse_date_text(text, days)) {
        cell.type_ = FT_DATE;
        cell.date_days_ = days;
      } else {
        ret = FS_ERR_BAD_DATA;
      }
      break;
    }
    case FT_DATETIME: {
      int64_t us = 0;
      if (FS_OK == parse_datetime_text(text, us)) {
        cell.type_ = FT_DATETIME;
        cell.datetime_us_ = us;
      } else if (FS_OK == parse_date_text(text, cell.date_days_)) {
        cell.type_ = FT_DATETIME;
        cell.datetime_us_ = static_cast<int64_t>(cell.date_days_) * 86400LL * 1000000LL;
      } else {
        ret = FS_ERR_BAD_DATA;
      }
      break;
    }
    case FT_VARCHAR:
    default: {
      cell.type_ = FT_VARCHAR;
      cell.str_val_ = text;
      break;
    }
  }
  return ret;
}

std::string csv_escape_field(const std::string &field, char delimiter, char quote, char escape)
{
  bool need_quote = false;
  for (size_t i = 0; i < field.size() && !need_quote; ++i) {
    const char c = field[i];
    need_quote = (c == delimiter || c == quote || c == '\n' || c == '\r');
  }
  std::string out;
  if (!need_quote) {
    out = field;
  } else {
    out.reserve(field.size() + 8);
    out.push_back(quote);
    for (size_t i = 0; i < field.size(); ++i) {
      const char c = field[i];
      if (c == quote || c == escape) {
        out.push_back(escape);
      }
      out.push_back(c);
    }
    out.push_back(quote);
  }
  return out;
}

int stat_file_signature(const std::string &path, FileSignature &sig, std::string &err)
{
  int ret = FS_OK;
  struct stat st;
  if (0 != stat(path.c_str(), &st)) {
    ret = ENOENT == errno ? FS_ERR_NOT_EXIST : FS_ERR_IO;
    set_err(err, "stat '%s' failed: %s", path.c_str(), strerror(errno));
  } else if (!S_ISREG(st.st_mode)) {
    ret = FS_ERR_INVALID_ARGUMENT;
    set_err(err, "'%s' is not a regular file", path.c_str());
  } else {
    sig.size_ = static_cast<int64_t>(st.st_size);
    sig.mtime_sec_ = static_cast<int64_t>(st.st_mtime);
    sig.mtime_nsec_ = static_cast<int64_t>(st.st_mtim.tv_nsec);
    sig.inode_ = static_cast<int64_t>(st.st_ino);
  }
  return ret;
}

bool is_regular_file(const std::string &path)
{
  struct stat st;
  return 0 == stat(path.c_str(), &st) && S_ISREG(st.st_mode);
}

int normalize_path(const std::string &path, std::string &normalized, std::string &err)
{
  int ret = FS_OK;
  char buf[PATH_MAX + 1];
  if (path.empty()) {
    ret = FS_ERR_INVALID_ARGUMENT;
    set_err(err, "empty file path");
  } else if (NULL == realpath(path.c_str(), buf)) {
    ret = ENOENT == errno ? FS_ERR_NOT_EXIST : FS_ERR_IO;
    set_err(err, "cannot resolve path '%s': %s", path.c_str(), strerror(errno));
  } else {
    normalized.assign(buf);
  }
  return ret;
}

int glob_files(const std::string &pattern,
               std::vector<std::string> &files,
               std::string &err)
{
  int ret = FS_OK;
  glob_t glob_result;
  memset(&glob_result, 0, sizeof(glob_result));
  const int rc = glob(pattern.c_str(), GLOB_NOSORT, NULL, &glob_result);
  if (GLOB_NOMATCH == rc) {
    ret = FS_OK;
  } else if (0 != rc) {
    ret = GLOB_NOSPACE == rc ? FS_ERR_IO : FS_ERR_INVALID_ARGUMENT;
    set_err(err, "glob '%s' failed", pattern.c_str());
  } else {
    for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
      const char *path = glob_result.gl_pathv[i];
      if (NULL != path && is_regular_file(path)) {
        files.push_back(std::string(path));
      }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
  }
  globfree(&glob_result);
  return ret;
}

namespace
{

//////////////////////////////////////////////////////////////////////////////
// CSV reader
//////////////////////////////////////////////////////////////////////////////
class CsvFileReader : public FileScanReader
{
public:
  CsvFileReader(const FileReadOptions &options, const std::string &path)
    : options_(options) { path_ = path; }
  virtual ~CsvFileReader() { close(); }
  virtual int infer_schema(std::vector<FileColumnSchema> &schema, std::string &err) override;
  virtual int open(std::string &err) override;
  virtual int next(FileRow &row, bool &eof, std::string &err) override;
  virtual void close() override;
  virtual int64_t estimated_rows() const override { return estimated_rows_; }
private:
  struct Field
  {
    std::string text_;
    bool quoted_ = false;
    void reset() { text_.clear(); quoted_ = false; }
  };
  int open_file(std::string &err);
  int read_char(char &c, bool &eof);
  int read_record(std::vector<Field> &fields, bool &eof, std::string &err);
  int build_schema(std::string &err);
  int cell_from_field(const Field &field, FileColumnType type, FileCell &cell, std::string &err);
  FileReadOptions options_;
  FILE *fp_ = NULL;
  int64_t line_no_ = 1;
  int64_t record_start_line_ = 1;
  int64_t estimated_rows_ = -1;
  bool schema_ready_ = false;
  bool header_skipped_ = false;
  std::vector<std::string> column_names_;
  std::vector<FileColumnType> column_types_;
  std::vector<int64_t> column_lengths_;
};

int CsvFileReader::open_file(std::string &err)
{
  int ret = FS_OK;
  close();
  if (NULL == (fp_ = fopen(path_.c_str(), "rb"))) {
    ret = ENOENT == errno ? FS_ERR_NOT_EXIST : FS_ERR_IO;
    set_err(err, "open '%s' failed: %s", path_.c_str(), strerror(errno));
  } else {
    line_no_ = 1;
    record_start_line_ = 1;
  }
  return ret;
}

void CsvFileReader::close()
{
  if (NULL != fp_) {
    fclose(fp_);
    fp_ = NULL;
  }
  header_skipped_ = false;
}

int CsvFileReader::read_char(char &c, bool &eof)
{
  int ret = FS_OK;
  const int ch = fgetc(fp_);
  if (EOF == ch) {
    if (ferror(fp_)) {
      ret = FS_ERR_IO;
    } else {
      eof = true;
    }
  } else {
    c = static_cast<char>(ch);
    eof = false;
  }
  return ret;
}

int CsvFileReader::read_record(std::vector<Field> &fields, bool &eof, std::string &err)
{
  int ret = FS_OK;
  Field field;
  bool in_quotes = false;
  bool after_quote = false;
  bool field_started = false;
  bool record_started = false;
  bool done = false;
  bool have_pending = false;
  char pending = '\0';
  fields.clear();
  eof = false;
  record_start_line_ = line_no_;
  while (FS_OK == ret && !done) {
    char c = '\0';
    bool at_eof = false;
    if (have_pending) {
      c = pending;
      have_pending = false;
      at_eof = false;
    } else if (FS_OK != (ret = read_char(c, at_eof))) {
    } else if (at_eof) {
      done = true;
      if (in_quotes) {
        ret = FS_ERR_BAD_DATA;
        set_err(err, "%s:%lld: unterminated quoted field",
                path_.c_str(), static_cast<long long>(record_start_line_));
      } else if (record_started || field_started || !field.text_.empty()) {
        fields.push_back(field);
      } else {
        eof = true;
      }
    }
    if (FS_OK != ret || at_eof || done) {
      continue;
    }
    if (in_quotes) {
      if (options_.escape_ != options_.quote_ && c == options_.escape_) {
        char next_c = '\0';
        bool next_eof = false;
        if (FS_OK != (ret = read_char(next_c, next_eof))) {
        } else if (next_eof) {
          ret = FS_ERR_BAD_DATA;
          set_err(err, "%s:%lld: escape character at end of file",
                  path_.c_str(), static_cast<long long>(line_no_));
        } else {
          if ('\n' == next_c) {
            ++line_no_;
          }
          field.text_.push_back(next_c);
        }
      } else if (c == options_.quote_) {
        if (options_.escape_ == options_.quote_) {
          char next_c = '\0';
          bool next_eof = false;
          if (FS_OK != (ret = read_char(next_c, next_eof))) {
          } else if (!next_eof && next_c == options_.quote_) {
            field.text_.push_back(options_.quote_);
          } else {
            in_quotes = false;
            after_quote = true;
            if (!next_eof) {
              have_pending = true;
              pending = next_c;
            }
          }
        } else {
          in_quotes = false;
          after_quote = true;
        }
      } else {
        if ('\n' == c) {
          ++line_no_;
        }
        field.text_.push_back(c);
      }
    } else if (after_quote) {
      if (c == options_.delimiter_) {
        fields.push_back(field);
        field.reset();
        field_started = false;
        after_quote = false;
        record_started = true;
      } else if ('\n' == c) {
        fields.push_back(field);
        ++line_no_;
        done = true;
      } else if ('\r' == c) {
        char next_c = '\0';
        bool next_eof = false;
        if (FS_OK != (ret = read_char(next_c, next_eof))) {
        } else if (!next_eof && '\n' != next_c) {
          have_pending = true;
          pending = next_c;
        }
        fields.push_back(field);
        ++line_no_;
        done = true;
      } else if (is_space(c) && c != '\r') {
        // tolerate trailing spaces after the closing quote
      } else {
        ret = FS_ERR_BAD_DATA;
        set_err(err, "%s:%lld: unexpected character '%c' after quoted field",
                path_.c_str(), static_cast<long long>(line_no_), c);
      }
    } else {
      if (!field_started && c == options_.quote_) {
        in_quotes = true;
        field_started = true;
        field.quoted_ = true;
        record_started = true;
      } else if (c == options_.delimiter_) {
        fields.push_back(field);
        field.reset();
        field_started = false;
        record_started = true;
      } else if ('\n' == c) {
        fields.push_back(field);
        ++line_no_;
        done = true;
      } else if ('\r' == c) {
        char next_c = '\0';
        bool next_eof = false;
        if (FS_OK != (ret = read_char(next_c, next_eof))) {
        } else if (!next_eof && '\n' != next_c) {
          have_pending = true;
          pending = next_c;
        }
        fields.push_back(field);
        ++line_no_;
        done = true;
      } else {
        field.text_.push_back(c);
        field_started = true;
        record_started = true;
      }
    }
  }
  return ret;
}

int CsvFileReader::build_schema(std::string &err)
{
  int ret = FS_OK;
  std::vector<Field> fields;
  bool eof = false;
  if (FS_OK != (ret = open_file(err))) {
  } else if (FS_OK != (ret = read_record(fields, eof, err))) {
  } else if (eof) {
    ret = FS_ERR_BAD_DATA;
    set_err(err, "%s: empty csv file", path_.c_str());
  } else if (options_.has_header_) {
    for (size_t i = 0; i < fields.size(); ++i) {
      std::string name = fields[i].text_;
      if (name.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "column%d", static_cast<int>(i));
        name.assign(buf);
      }
      column_names_.push_back(name);
    }
  } else {
    for (size_t i = 0; i < fields.size(); ++i) {
      char buf[32];
      snprintf(buf, sizeof(buf), "column%d", static_cast<int>(i));
      column_names_.push_back(std::string(buf));
    }
  }
  const size_t column_cnt = column_names_.size();
  // de-duplicate column names
  for (size_t i = 0; i < column_cnt; ++i) {
    const std::string base_name = column_names_[i];
    int suffix = 1;
    bool duplicated = true;
    while (duplicated) {
      duplicated = false;
      for (size_t j = 0; j < i; ++j) {
        if (column_names_[j] == column_names_[i]) {
          duplicated = true;
          break;
        }
      }
      if (duplicated) {
        char buf[64];
        snprintf(buf, sizeof(buf), "_%d", ++suffix);
        column_names_[i] = base_name + buf;
      }
    }
  }
  column_types_.assign(column_cnt, FT_NULL);
  column_lengths_.assign(column_cnt, 0);
  int64_t row_cnt = 0;
  if (FS_OK == ret && !options_.has_header_) {
    // the first record was consumed for column names, feed it into inference
    if (fields.size() != column_cnt) {
      ret = FS_ERR_BAD_DATA;
      set_err(err, "%s:%lld: expected %zu columns but got %zu", path_.c_str(),
              static_cast<long long>(line_no_), column_cnt, fields.size());
    }
    for (size_t i = 0; FS_OK == ret && i < fields.size(); ++i) {
      int64_t len = column_lengths_[i];
      const FileColumnType type = infer_text_type(fields[i].text_, len);
      column_lengths_[i] = len;
      column_types_[i] = merge_column_type(column_types_[i], type);
    }
    ++row_cnt;
  }
  while (FS_OK == ret && !eof) {
    if (FS_OK != (ret = read_record(fields, eof, err))) {
    } else if (eof) {
      break;
    } else if (1 == fields.size() && !fields[0].quoted_ && fields[0].text_.empty()) {
      continue;
    } else if (fields.size() != column_cnt) {
      ret = FS_ERR_BAD_DATA;
      set_err(err, "%s:%lld: expected %zu columns but got %zu", path_.c_str(),
              static_cast<long long>(record_start_line_), column_cnt, fields.size());
    } else {
      for (size_t i = 0; FS_OK == ret && i < fields.size(); ++i) {
        int64_t len = column_lengths_[i];
        const FileColumnType type = infer_text_type(fields[i].text_, len);
        column_lengths_[i] = len;
        column_types_[i] = merge_column_type(column_types_[i], type);
      }
      ++row_cnt;
    }
  }
  estimated_rows_ = row_cnt;
  close();
  return ret;
}

int CsvFileReader::infer_schema(std::vector<FileColumnSchema> &schema, std::string &err)
{
  int ret = FS_OK;
  if (FS_OK != (ret = build_schema(err))) {
  } else {
    schema.clear();
    for (size_t i = 0; i < column_names_.size(); ++i) {
      FileColumnSchema col;
      col.name_ = column_names_[i];
      col.type_ = column_types_[i];
      col.varchar_length_ = column_types_[i] == FT_VARCHAR ? column_lengths_[i] : 0;
      col.nullable_ = true;
      schema.push_back(col);
    }
    schema_ready_ = true;
  }
  return ret;
}

int CsvFileReader::open(std::string &err)
{
  int ret = FS_OK;
  std::vector<Field> fields;
  bool eof = false;
  if (!schema_ready_) {
    ret = FS_ERR_INVALID_ARGUMENT;
    set_err(err, "csv reader schema not inferred");
  } else if (FS_OK != (ret = open_file(err))) {
  } else if (options_.has_header_ && FS_OK != (ret = read_record(fields, eof, err))) {
  } else if (options_.has_header_ && eof) {
    ret = FS_ERR_BAD_DATA;
    set_err(err, "%s: empty csv file", path_.c_str());
  }
  return ret;
}

int CsvFileReader::cell_from_field(const Field &field, FileColumnType type,
                                   FileCell &cell, std::string &err)
{
  int ret = FS_OK;
  const bool is_null = !field.quoted_ &&
                       ((options_.null_str_set_ && field.text_ == options_.null_str_) ||
                        (!options_.null_str_set_ && field.text_.empty()));
  if (is_null) {
    cell.reset();
  } else if (FS_OK != (ret = parse_text_to_cell(field.text_, type, cell))) {
    set_err(err, "%s:%lld: cannot convert '%.64s' to %s",
            path_.c_str(), static_cast<long long>(record_start_line_),
            field.text_.c_str(), file_column_type_name(type));
  }
  return ret;
}

int CsvFileReader::next(FileRow &row, bool &eof, std::string &err)
{
  int ret = FS_OK;
  std::vector<Field> fields;
  row.clear();
  if (FS_OK != (ret = read_record(fields, eof, err))) {
  } else if (eof) {
  } else if (1 == fields.size() && !fields[0].quoted_ && fields[0].text_.empty()) {
    ret = next(row, eof, err);
  } else if (fields.size() != column_types_.size()) {
    ret = FS_ERR_BAD_DATA;
    set_err(err, "%s:%lld: expected %zu columns but got %zu", path_.c_str(),
            static_cast<long long>(record_start_line_), column_types_.size(), fields.size());
  } else {
    row.resize(fields.size());
    for (size_t i = 0; FS_OK == ret && i < fields.size(); ++i) {
      ret = cell_from_field(fields[i], column_types_[i], row[i], err);
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////
// JSON lines reader
//////////////////////////////////////////////////////////////////////////////
class JsonValue
{
public:
  enum Kind { J_NULL = 0, J_BOOL, J_INT, J_DOUBLE, J_STRING, J_OBJECT, J_ARRAY };
  Kind kind_ = J_NULL;
  bool bool_val_ = false;
  int64_t int_val_ = 0;
  double double_val_ = 0.0;
  std::string str_val_;
  std::vector<std::pair<std::string, std::shared_ptr<JsonValue> > > obj_;
  std::vector<std::shared_ptr<JsonValue> > arr_;
  void reset() { *this = JsonValue(); }
};

class JsonParser
{
public:
  JsonParser(const char *data, int64_t len) : p_(data), begin_(data), end_(data + len) {}
  int parse(JsonValue &value, std::string &err);
  int64_t error_offset() const { return error_offset_; }
private:
  int skip_space();
  int parse_value(JsonValue &value, int depth);
  int parse_string(std::string &str);
  int parse_number(JsonValue &value);
  int parse_literal(const char *literal);
  const char *p_;
  const char *begin_;
  const char *end_;
  int64_t error_offset_ = 0;
};

int JsonParser::skip_space()
{
  while (p_ < end_ && (' ' == *p_ || '\t' == *p_ || '\r' == *p_ || '\n' == *p_)) {
    ++p_;
  }
  return FS_OK;
}

int JsonParser::parse_literal(const char *literal)
{
  int ret = FS_OK;
  const int64_t len = static_cast<int64_t>(strlen(literal));
  if (end_ - p_ < len || 0 != memcmp(p_, literal, static_cast<size_t>(len))) {
    error_offset_ = p_ - begin_;
    ret = FS_ERR_BAD_DATA;
  } else {
    p_ += len;
  }
  return ret;
}

int JsonParser::parse_string(std::string &str)
{
  int ret = FS_OK;
  str.clear();
  if (p_ >= end_ || '"' != *p_) {
    ret = FS_ERR_BAD_DATA;
  } else {
    ++p_;
    bool done = false;
    while (FS_OK == ret && !done) {
      if (p_ >= end_) {
        ret = FS_ERR_BAD_DATA;
      } else {
        const unsigned char c = static_cast<unsigned char>(*p_);
        if ('"' == c) {
          ++p_;
          done = true;
        } else if ('\\' == c) {
          ++p_;
          if (p_ >= end_) {
            ret = FS_ERR_BAD_DATA;
          } else {
            const char esc = *p_++;
            switch (esc) {
              case '"': str.push_back('"'); break;
              case '\\': str.push_back('\\'); break;
              case '/': str.push_back('/'); break;
              case 'b': str.push_back('\b'); break;
              case 'f': str.push_back('\f'); break;
              case 'n': str.push_back('\n'); break;
              case 'r': str.push_back('\r'); break;
              case 't': str.push_back('\t'); break;
              case 'u': {
                if (end_ - p_ < 4) {
                  ret = FS_ERR_BAD_DATA;
                } else {
                  uint32_t code = 0;
                  for (int i = 0; FS_OK == ret && i < 4; ++i) {
                    const char h = *p_++;
                    code <<= 4;
                    if (h >= '0' && h <= '9') {
                      code |= static_cast<uint32_t>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                      code |= static_cast<uint32_t>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                      code |= static_cast<uint32_t>(h - 'A' + 10);
                    } else {
                      ret = FS_ERR_BAD_DATA;
                    }
                  }
                  if (FS_OK == ret) {
                    // surrogate pair
                    if (code >= 0xD800 && code <= 0xDBFF && end_ - p_ >= 6 &&
                        '\\' == p_[0] && 'u' == p_[1]) {
                      p_ += 2;
                      uint32_t low = 0;
                      for (int i = 0; FS_OK == ret && i < 4; ++i) {
                        const char h = *p_++;
                        low <<= 4;
                        if (h >= '0' && h <= '9') {
                          low |= static_cast<uint32_t>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                          low |= static_cast<uint32_t>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                          low |= static_cast<uint32_t>(h - 'A' + 10);
                        } else {
                          ret = FS_ERR_BAD_DATA;
                        }
                      }
                      if (FS_OK == ret) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                      }
                    }
                    if (FS_OK == ret) {
                      if (code < 0x80) {
                        str.push_back(static_cast<char>(code));
                      } else if (code < 0x800) {
                        str.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        str.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                      } else if (code < 0x10000) {
                        str.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        str.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        str.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                      } else {
                        str.push_back(static_cast<char>(0xF0 | (code >> 18)));
                        str.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                        str.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        str.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                      }
                    }
                  }
                }
                break;
              }
              default:
                ret = FS_ERR_BAD_DATA;
                break;
            }
          }
        } else if (c < 0x20) {
          ret = FS_ERR_BAD_DATA;
        } else {
          str.push_back(static_cast<char>(c));
          ++p_;
        }
      }
    }
  }
  return ret;
}

int JsonParser::parse_number(JsonValue &value)
{
  int ret = FS_OK;
  const char *start = p_;
  bool is_double = false;
  if (p_ < end_ && ('-' == *p_ || '+' == *p_)) {
    ++p_;
  }
  while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
    ++p_;
  }
  if (p_ < end_ && '.' == *p_) {
    is_double = true;
    ++p_;
    while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
      ++p_;
    }
  }
  if (p_ < end_ && ('e' == *p_ || 'E' == *p_)) {
    is_double = true;
    ++p_;
    if (p_ < end_ && ('-' == *p_ || '+' == *p_)) {
      ++p_;
    }
    while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
      ++p_;
    }
  }
  if (p_ == start || (1 == p_ - start && ('-' == *start || '+' == *start))) {
    ret = FS_ERR_BAD_DATA;
  } else {
    const std::string text(start, static_cast<size_t>(p_ - start));
    int64_t int_value = 0;
    double double_value = 0.0;
    if (!is_double && parse_int64_strict(text, int_value)) {
      value.kind_ = JsonValue::J_INT;
      value.int_val_ = int_value;
    } else if (parse_double_strict(text, double_value)) {
      value.kind_ = JsonValue::J_DOUBLE;
      value.double_val_ = double_value;
    } else {
      ret = FS_ERR_BAD_DATA;
    }
  }
  return ret;
}

int JsonParser::parse_value(JsonValue &value, int depth)
{
  int ret = FS_OK;
  value.reset();
  if (depth > 64) {
    ret = FS_ERR_BAD_DATA;
  } else if (FS_OK != (ret = skip_space())) {
  } else if (p_ >= end_) {
    ret = FS_ERR_BAD_DATA;
  } else {
    const char c = *p_;
    if ('{' == c) {
      value.kind_ = JsonValue::J_OBJECT;
      ++p_;
      if (FS_OK != (ret = skip_space())) {
      } else if (p_ < end_ && '}' == *p_) {
        ++p_;
      } else {
        bool done = false;
        while (FS_OK == ret && !done) {
          std::string key;
          JsonValue child;
          if (FS_OK != (ret = skip_space())) {
          } else if (FS_OK != (ret = parse_string(key))) {
          } else if (FS_OK != (ret = skip_space()) || p_ >= end_ || ':' != *p_) {
            ret = FS_ERR_BAD_DATA;
          } else {
            ++p_;
            if (FS_OK != (ret = parse_value(child, depth + 1))) {
            } else {
              value.obj_.push_back(std::make_pair(key, std::make_shared<JsonValue>(std::move(child))));
              if (FS_OK != (ret = skip_space())) {
              } else if (p_ < end_ && ',' == *p_) {
                ++p_;
              } else if (p_ < end_ && '}' == *p_) {
                ++p_;
                done = true;
              } else {
                ret = FS_ERR_BAD_DATA;
              }
            }
          }
        }
      }
    } else if ('[' == c) {
      value.kind_ = JsonValue::J_ARRAY;
      ++p_;
      if (FS_OK != (ret = skip_space())) {
      } else if (p_ < end_ && ']' == *p_) {
        ++p_;
      } else {
        bool done = false;
        while (FS_OK == ret && !done) {
          JsonValue child;
          if (FS_OK != (ret = parse_value(child, depth + 1))) {
          } else {
            value.arr_.push_back(std::make_shared<JsonValue>(std::move(child)));
            if (FS_OK != (ret = skip_space())) {
            } else if (p_ < end_ && ',' == *p_) {
              ++p_;
            } else if (p_ < end_ && ']' == *p_) {
              ++p_;
              done = true;
            } else {
              ret = FS_ERR_BAD_DATA;
            }
          }
        }
      }
    } else if ('"' == c) {
      value.kind_ = JsonValue::J_STRING;
      ret = parse_string(value.str_val_);
    } else if ('t' == c) {
      value.kind_ = JsonValue::J_BOOL;
      value.bool_val_ = true;
      ret = parse_literal("true");
    } else if ('f' == c) {
      value.kind_ = JsonValue::J_BOOL;
      value.bool_val_ = false;
      ret = parse_literal("false");
    } else if ('n' == c) {
      value.kind_ = JsonValue::J_NULL;
      ret = parse_literal("null");
    } else {
      ret = parse_number(value);
    }
  }
  return ret;
}

int JsonParser::parse(JsonValue &value, std::string &err)
{
  int ret = FS_OK;
  if (FS_OK != (ret = parse_value(value, 0))) {
    char buf[128];
    snprintf(buf, sizeof(buf), "invalid json at column %lld",
             static_cast<long long>(p_ - begin_ + 1));
    err.assign(buf);
  } else if (FS_OK != (ret = skip_space()) || p_ != end_) {
    ret = FS_ERR_BAD_DATA;
    err.assign("trailing characters after json value");
  }
  return ret;
}

class JsonlFileReader : public FileScanReader
{
public:
  JsonlFileReader(const FileReadOptions &options, const std::string &path)
    : options_(options) { path_ = path; }
  virtual ~JsonlFileReader() { close(); }
  virtual int infer_schema(std::vector<FileColumnSchema> &schema, std::string &err) override;
  virtual int open(std::string &err) override;
  virtual int next(FileRow &row, bool &eof, std::string &err) override;
  virtual void close() override;
  virtual int64_t estimated_rows() const override { return estimated_rows_; }
private:
  int open_file(std::string &err);
  int read_line(std::string &line, bool &eof, std::string &err);
  int build_schema(std::string &err);
  int json_to_cell(const JsonValue &value, FileColumnType type, FileCell &cell,
                   int64_t line_no, const std::string &field, std::string &err);
  FileReadOptions options_;
  FILE *fp_ = NULL;
  int64_t line_no_ = 0;
  int64_t estimated_rows_ = -1;
  bool schema_ready_ = false;
  std::vector<std::string> column_names_;
  std::vector<FileColumnType> column_types_;
  std::vector<int64_t> column_lengths_;
};

int JsonlFileReader::open_file(std::string &err)
{
  int ret = FS_OK;
  close();
  if (NULL == (fp_ = fopen(path_.c_str(), "rb"))) {
    ret = ENOENT == errno ? FS_ERR_NOT_EXIST : FS_ERR_IO;
    set_err(err, "open '%s' failed: %s", path_.c_str(), strerror(errno));
  } else {
    line_no_ = 0;
  }
  return ret;
}

void JsonlFileReader::close()
{
  if (NULL != fp_) {
    fclose(fp_);
    fp_ = NULL;
  }
}

int JsonlFileReader::read_line(std::string &line, bool &eof, std::string &err)
{
  int ret = FS_OK;
  line.clear();
  eof = false;
  bool done = false;
  while (FS_OK == ret && !done) {
    const int c = fgetc(fp_);
    if (EOF == c) {
      if (ferror(fp_)) {
        ret = FS_ERR_IO;
        set_err(err, "read '%s' failed", path_.c_str());
      } else if (line.empty()) {
        eof = true;
      } else {
        ++line_no_;
      }
      done = true;
    } else if ('\n' == c) {
      ++line_no_;
      done = true;
    } else {
      line.push_back(static_cast<char>(c));
      if (static_cast<int64_t>(line.size()) > options_.max_line_size_) {
        ret = FS_ERR_BAD_DATA;
        set_err(err, "%s:%lld: line exceeds maximum size %lld", path_.c_str(),
                static_cast<long long>(line_no_ + 1),
                static_cast<long long>(options_.max_line_size_));
      }
    }
  }
  if (FS_OK == ret && !line.empty() && '\r' == line[line.size() - 1]) {
    line.erase(line.size() - 1);
  }
  return ret;
}

int JsonlFileReader::build_schema(std::string &err)
{
  int ret = FS_OK;
  std::string line;
  bool eof = false;
  int64_t row_cnt = 0;
  if (FS_OK != (ret = open_file(err))) {
  }
  while (FS_OK == ret && !eof) {
    if (FS_OK != (ret = read_line(line, eof, err))) {
    } else if (eof || line.empty()) {
      continue;
    } else {
      JsonValue value;
      JsonParser parser(line.data(), static_cast<int64_t>(line.size()));
      std::string parse_err;
      if (FS_OK != (ret = parser.parse(value, parse_err))) {
        set_err(err, "%s:%lld: %s", path_.c_str(),
                static_cast<long long>(line_no_), parse_err.c_str());
      } else if (JsonValue::J_OBJECT != value.kind_) {
        ret = FS_ERR_BAD_DATA;
        set_err(err, "%s:%lld: each line must be a json object", path_.c_str(),
                static_cast<long long>(line_no_));
      } else {
        for (size_t i = 0; FS_OK == ret && i < value.obj_.size(); ++i) {
          const std::string &key = value.obj_[i].first;
          const JsonValue &child = *value.obj_[i].second;
          int64_t col = -1;
          for (size_t j = 0; j < column_names_.size(); ++j) {
            if (column_names_[j] == key) {
              col = static_cast<int64_t>(j);
              break;
            }
          }
          if (-1 == col) {
            column_names_.push_back(key);
            column_types_.push_back(FT_NULL);
            column_lengths_.push_back(0);
            col = static_cast<int64_t>(column_names_.size() - 1);
          }
          FileColumnType type = FT_NULL;
          int64_t len = column_lengths_[col];
          switch (child.kind_) {
            case JsonValue::J_NULL: type = FT_NULL; break;
            case JsonValue::J_BOOL: type = FT_BOOLEAN; break;
            case JsonValue::J_INT: type = FT_BIGINT; break;
            case JsonValue::J_DOUBLE: type = FT_DOUBLE; break;
            case JsonValue::J_STRING:
              type = FT_VARCHAR;
              if (static_cast<int64_t>(child.str_val_.size()) > len) {
                len = static_cast<int64_t>(child.str_val_.size());
              }
              break;
            case JsonValue::J_OBJECT:
            case JsonValue::J_ARRAY:
            default:
              ret = FS_ERR_UNSUPPORTED;
              set_err(err, "%s:%lld: field '%s': nested %s is not supported",
                      path_.c_str(), static_cast<long long>(line_no_), key.c_str(),
                      JsonValue::J_OBJECT == child.kind_ ? "object" : "array values");
              break;
          }
          if (FS_OK == ret) {
            column_lengths_[col] = len;
            column_types_[col] = merge_column_type(column_types_[col], type);
          }
        }
        ++row_cnt;
      }
    }
  }
  estimated_rows_ = row_cnt;
  close();
  return ret;
}

int JsonlFileReader::infer_schema(std::vector<FileColumnSchema> &schema, std::string &err)
{
  int ret = FS_OK;
  if (FS_OK != (ret = build_schema(err))) {
  } else {
    schema.clear();
    for (size_t i = 0; i < column_names_.size(); ++i) {
      FileColumnSchema col;
      col.name_ = column_names_[i];
      col.type_ = column_types_[i];
      col.varchar_length_ = column_types_[i] == FT_VARCHAR ? column_lengths_[i] : 0;
      col.nullable_ = true;
      schema.push_back(col);
    }
    schema_ready_ = true;
  }
  return ret;
}

int JsonlFileReader::open(std::string &err)
{
  int ret = FS_OK;
  if (!schema_ready_) {
    ret = FS_ERR_INVALID_ARGUMENT;
    set_err(err, "jsonl reader schema not inferred");
  } else {
    ret = open_file(err);
  }
  return ret;
}

int JsonlFileReader::json_to_cell(const JsonValue &value, FileColumnType type, FileCell &cell,
                                  int64_t line_no, const std::string &field, std::string &err)
{
  int ret = FS_OK;
  cell.reset();
  switch (value.kind_) {
    case JsonValue::J_NULL:
      cell.type_ = FT_NULL;
      break;
    case JsonValue::J_BOOL:
      cell.type_ = FT_BOOLEAN;
      cell.bool_val_ = value.bool_val_;
      break;
    case JsonValue::J_INT:
      if (FT_DOUBLE == type) {
        cell.type_ = FT_DOUBLE;
        cell.double_val_ = static_cast<double>(value.int_val_);
      } else if (FT_VARCHAR == type) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.int_val_));
        cell.type_ = FT_VARCHAR;
        cell.str_val_ = buf;
      } else {
        cell.type_ = FT_BIGINT;
        cell.int_val_ = value.int_val_;
      }
      break;
    case JsonValue::J_DOUBLE:
      if (FT_VARCHAR == type) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", value.double_val_);
        cell.type_ = FT_VARCHAR;
        cell.str_val_ = buf;
      } else {
        cell.type_ = FT_DOUBLE;
        cell.double_val_ = value.double_val_;
      }
      break;
    case JsonValue::J_STRING: {
      if (FT_DATE == type) {
        if (FS_OK != parse_date_text(value.str_val_, cell.date_days_)) {
          ret = FS_ERR_BAD_DATA;
        } else {
          cell.type_ = FT_DATE;
        }
      } else if (FT_DATETIME == type) {
        if (FS_OK != parse_datetime_text(value.str_val_, cell.datetime_us_)) {
          ret = FS_ERR_BAD_DATA;
        } else {
          cell.type_ = FT_DATETIME;
        }
      } else if (FT_BIGINT == type) {
        if (!parse_int64_strict(value.str_val_, cell.int_val_)) {
          ret = FS_ERR_BAD_DATA;
        } else {
          cell.type_ = FT_BIGINT;
        }
      } else if (FT_DOUBLE == type) {
        if (!parse_double_strict(value.str_val_, cell.double_val_)) {
          ret = FS_ERR_BAD_DATA;
        } else {
          cell.type_ = FT_DOUBLE;
        }
      } else if (FT_BOOLEAN == type) {
        std::string lower;
        for (size_t i = 0; i < value.str_val_.size(); ++i) {
          lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(value.str_val_[i]))));
        }
        if ("true" == lower) {
          cell.type_ = FT_BOOLEAN;
          cell.bool_val_ = true;
        } else if ("false" == lower) {
          cell.type_ = FT_BOOLEAN;
          cell.bool_val_ = false;
        } else {
          ret = FS_ERR_BAD_DATA;
        }
      } else {
        cell.type_ = FT_VARCHAR;
        cell.str_val_ = value.str_val_;
      }
      if (FS_OK != ret) {
        set_err(err, "%s:%lld: field '%s': cannot convert '%s' to %s", path_.c_str(),
                static_cast<long long>(line_no), field.c_str(),
                value.str_val_.c_str(), file_column_type_name(type));
      }
      break;
    }
    default:
      ret = FS_ERR_UNSUPPORTED;
      set_err(err, "%s:%lld: field '%s': nested value is not supported", path_.c_str(),
              static_cast<long long>(line_no), field.c_str());
      break;
  }
  return ret;
}

int JsonlFileReader::next(FileRow &row, bool &eof, std::string &err)
{
  int ret = FS_OK;
  std::string line;
  row.clear();
  bool got_row = false;
  while (FS_OK == ret && !got_row && !eof) {
    if (FS_OK != (ret = read_line(line, eof, err))) {
    } else if (eof) {
    } else if (line.empty()) {
      continue;
    } else {
      JsonValue value;
      JsonParser parser(line.data(), static_cast<int64_t>(line.size()));
      std::string parse_err;
      if (FS_OK != (ret = parser.parse(value, parse_err))) {
        set_err(err, "%s:%lld: %s", path_.c_str(),
                static_cast<long long>(line_no_), parse_err.c_str());
      } else if (JsonValue::J_OBJECT != value.kind_) {
        ret = FS_ERR_BAD_DATA;
        set_err(err, "%s:%lld: each line must be a json object", path_.c_str(),
                static_cast<long long>(line_no_));
      } else {
        row.resize(column_names_.size());
        for (size_t i = 0; FS_OK == ret && i < column_names_.size(); ++i) {
          const JsonValue *child = NULL;
          for (size_t j = 0; j < value.obj_.size(); ++j) {
            if (value.obj_[j].first == column_names_[i]) {
              child = value.obj_[j].second.get();
              break;
            }
          }
          if (NULL == child) {
            row[i].reset();
          } else {
            ret = json_to_cell(*child, column_types_[i], row[i], line_no_,
                               column_names_[i], err);
          }
        }
        got_row = true;
      }
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////
// glob reader
//////////////////////////////////////////////////////////////////////////////
class GlobFileReader : public FileScanReader
{
public:
  GlobFileReader(const FileReadOptions &options, const std::string &path)
    : options_(options) { path_ = path; }
  virtual ~GlobFileReader() {}
  virtual int infer_schema(std::vector<FileColumnSchema> &schema, std::string &err) override
  {
    (void)err;
    schema.clear();
    FileColumnSchema col;
    col.name_ = "file";
    col.type_ = FT_VARCHAR;
    col.varchar_length_ = 1024;
    col.nullable_ = false;
    schema.push_back(col);
    return FS_OK;
  }
  virtual int open(std::string &err) override
  {
    files_.clear();
    idx_ = 0;
    return glob_files(path_, files_, err);
  }
  virtual int next(FileRow &row, bool &eof, std::string &err) override
  {
    (void)err;
    int ret = FS_OK;
    row.clear();
    if (idx_ >= files_.size()) {
      eof = true;
    } else {
      FileCell cell;
      cell.type_ = FT_VARCHAR;
      cell.str_val_ = files_[idx_++];
      row.push_back(cell);
      eof = false;
    }
    return ret;
  }
  virtual void close() override
  {
    files_.clear();
    idx_ = 0;
  }
  virtual int64_t estimated_rows() const override
  {
    return static_cast<int64_t>(files_.size());
  }
  virtual bool is_glob() const override { return true; }
private:
  FileReadOptions options_;
  std::vector<std::string> files_;
  size_t idx_ = 0;
};

} // anonymous namespace

int create_file_scan_reader(const FileReadOptions &options,
                            const std::string &path,
                            FileScanReader *&reader,
                            std::string &err)
{
  int ret = FS_OK;
  reader = NULL;
  std::string format = options.format_;
  for (size_t i = 0; i < format.size(); ++i) {
    format[i] = static_cast<char>(tolower(static_cast<unsigned char>(format[i])));
  }
  if ("csv" == format || "json" == format || "ndjson" == format || "jsonl" == format) {
    if ("csv" == format) {
      reader = new (std::nothrow) CsvFileReader(options, path);
    } else {
      reader = new (std::nothrow) JsonlFileReader(options, path);
    }
    if (NULL == reader) {
      ret = FS_ERR_IO;
      set_err(err, "allocate file reader failed");
    } else {
      reader->set_path(path);
    }
  } else if ("parquet" == format) {
    ret = create_parquet_reader(options, path, reader, err);
  } else if ("glob" == format) {
    reader = new (std::nothrow) GlobFileReader(options, path);
    if (NULL == reader) {
      ret = FS_ERR_IO;
      set_err(err, "allocate file reader failed");
    } else {
      reader->set_path(path);
    }
  } else {
    ret = FS_ERR_UNSUPPORTED;
    set_err(err, "unsupported file format '%s'", options.format_.c_str());
  }
  return ret;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
