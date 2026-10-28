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

#include "sql/engine/basic/ob_file_table_access_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"

using namespace oceanbase::common;
using namespace oceanbase::sql::file_sql;

namespace oceanbase
{
namespace sql
{

OB_SERIALIZE_MEMBER_INHERIT(ObFileTableAccessSpec,
                            ObOpSpec,
                            path_,
                            format_,
                            delimiter_,
                            quote_,
                            escape_,
                            has_header_,
                            null_str_,
                            row_count_,
                            file_size_,
                            file_mtime_sec_,
                            file_mtime_nsec_,
                            file_inode_,
                            column_exprs_);

ObFileTableAccessOp::~ObFileTableAccessOp()
{
  close_reader();
}

void ObFileTableAccessOp::destroy()
{
  close_reader();
  ObOperator::destroy();
}

int ObFileTableAccessOp::build_read_options(FileReadOptions &options) const
{
  int ret = OB_SUCCESS;
  options.reset();
  switch (MY_SPEC.format_) {
    case 1: options.format_ = "csv"; break;
    case 2: options.format_ = "jsonl"; break;
    case 3: options.format_ = "parquet"; break;
    case 4: options.format_ = "glob"; break;
    default:
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected file table format", K(MY_SPEC.format_), K(ret));
      break;
  }
  options.delimiter_ = static_cast<char>(MY_SPEC.delimiter_);
  options.quote_ = static_cast<char>(MY_SPEC.quote_);
  options.escape_ = static_cast<char>(MY_SPEC.escape_);
  options.has_header_ = MY_SPEC.has_header_;
  options.sample_size_ = -1;
  if (!MY_SPEC.null_str_.empty()) {
    options.null_str_set_ = true;
    options.null_str_.assign(MY_SPEC.null_str_.ptr(), MY_SPEC.null_str_.length());
  }
  return ret;
}

int ObFileTableAccessOp::verify_file_signature()
{
  int ret = OB_SUCCESS;
  std::string path(MY_SPEC.path_.ptr(), MY_SPEC.path_.length());
  FileSignature sig;
  std::string err;
  if (0 == MY_SPEC.format_ || 4 == MY_SPEC.format_) {
    // glob table only produces the file list, the list may change with the dir
  } else if (OB_FAIL(stat_file_signature(path, sig, err))) {
    ret = file_sql::file_scan_ret_to_ob_error(ret);
    LOG_WARN("failed to stat file table", K(path.c_str()), K(err.c_str()), K(ret));
  } else if (sig.size_ != MY_SPEC.file_size_ ||
             sig.mtime_sec_ != MY_SPEC.file_mtime_sec_ ||
             sig.mtime_nsec_ != MY_SPEC.file_mtime_nsec_ ||
             sig.inode_ != MY_SPEC.file_inode_) {
    ret = OB_INVALID_DATA;
    LOG_WARN("file table file has been changed after schema inference", K(path.c_str()),
             K(sig.size_), K(MY_SPEC.file_size_), K(sig.mtime_sec_), K(MY_SPEC.file_mtime_sec_),
             K(ret));
    LOG_USER_ERROR(OB_INVALID_DATA);
  }
  return ret;
}

namespace
{
// Report a file scan failure to the client with a meaningful error code/message.
void log_file_scan_error(int ob_ret)
{
  if (OB_NOT_SUPPORTED == ob_ret) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested object or array in file table");
  } else if (OB_FILE_NOT_EXIST == ob_ret) {
    LOG_USER_ERROR(OB_FILE_NOT_EXIST);
  } else if (OB_INVALID_ARGUMENT == ob_ret) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file table option or data");
  } else {
    LOG_USER_ERROR(OB_INVALID_DATA);
  }
}
} // anonymous namespace

int ObFileTableAccessOp::open_reader()
{
  int ret = OB_SUCCESS;
  std::string path(MY_SPEC.path_.ptr(), MY_SPEC.path_.length());
  std::string err;
  FileReadOptions options;
  std::vector<FileColumnSchema> schema;
  if (OB_NOT_NULL(reader_)) {
    reader_->close();
    delete reader_;
    reader_ = NULL;
  }
  if (OB_FAIL(build_read_options(options))) {
  } else if (OB_FAIL(create_file_scan_reader(options, path, reader_, err))) {
    ret = file_sql::file_scan_ret_to_ob_error(ret);
    LOG_WARN("failed to create file scan reader", K(path.c_str()), K(err.c_str()), K(ret));
    log_file_scan_error(ret);
  } else if (OB_ISNULL(reader_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(reader_->infer_schema(schema, err))) {
    ret = file_sql::file_scan_ret_to_ob_error(ret);
    LOG_WARN("failed to infer file scan schema at runtime", K(path.c_str()), K(err.c_str()), K(ret));
    log_file_scan_error(ret);
    close_reader();
  } else if (OB_FAIL(reader_->open(err))) {
    ret = file_sql::file_scan_ret_to_ob_error(ret);
    LOG_WARN("failed to open file scan reader", K(path.c_str()), K(err.c_str()), K(ret));
    log_file_scan_error(ret);
    close_reader();
  } else {
    opened_ = true;
  }
  return ret;
}

int ObFileTableAccessOp::close_reader()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(reader_)) {
    reader_->close();
    delete reader_;
    reader_ = NULL;
  }
  opened_ = false;
  return ret;
}

int ObFileTableAccessOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(MY_SPEC.column_exprs_.empty()) || OB_UNLIKELY(MY_SPEC.path_.empty())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid file table access spec", K(MY_SPEC.column_exprs_.count()), K(ret));
  } else if (OB_ISNULL(GET_MY_SESSION(ctx_))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    date_sql_mode_ = ObDateSqlMode(GET_MY_SESSION(ctx_)->get_sql_mode());
  }
  if (OB_SUCC(ret) && OB_FAIL(verify_file_signature())) {
  } else if (OB_SUCC(ret) && OB_FAIL(open_reader())) {
  }
  return ret;
}

int ObFileTableAccessOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (OB_FAIL(close_reader())) {
  } else if (OB_FAIL(verify_file_signature())) {
  } else if (OB_FAIL(open_reader())) {
  }
  return ret;
}

int ObFileTableAccessOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(close_reader())) {
  }
  return ret;
}

int ObFileTableAccessOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  bool eof = false;
  std::string err;
  if (OB_FAIL(try_check_status())) {
  } else if (OB_ISNULL(reader_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table reader is not opened", K(ret));
  } else if (OB_FAIL(reader_->next(row_, eof, err))) {
    ret = file_sql::file_scan_ret_to_ob_error(ret);
    LOG_WARN("failed to read file table row", K(err.c_str()), K(ret));
    log_file_scan_error(ret);
  } else if (eof) {
    ret = OB_ITER_END;
  } else {
    clear_evaluated_flag();
    if (OB_FAIL(fill_row(row_))) {
      LOG_WARN("failed to fill file table row", K(ret));
    }
  }
  return ret;
}

int ObFileTableAccessOp::fill_row(const FileRow &row)
{
  int ret = OB_SUCCESS;
  const int64_t output_cnt = MY_SPEC.column_exprs_.count();
  if (OB_UNLIKELY(static_cast<int64_t>(row.size()) < output_cnt)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("file row has less columns than inferred schema",
             "row_column_cnt", row.size(), K(output_cnt), K(ret));
    LOG_USER_ERROR(OB_INVALID_DATA);
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < output_cnt; ++i) {
    ObExpr *expr = MY_SPEC.column_exprs_.at(i);
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(set_cell_datum(expr, row.at(i)))) {
    } else {
      expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}

int ObFileTableAccessOp::set_cell_datum(ObExpr *expr, const FileCell &cell)
{
  int ret = OB_SUCCESS;
  ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
  switch (cell.type_) {
    case FT_NULL: {
      datum.set_null();
      break;
    }
    case FT_BOOLEAN: {
      datum.set_bool(cell.bool_val_);
      break;
    }
    case FT_BIGINT: {
      datum.set_int(cell.int_val_);
      break;
    }
    case FT_DOUBLE: {
      datum.set_double(cell.double_val_);
      break;
    }
    case FT_VARCHAR: {
      ObDatum tmp;
      tmp.set_string(cell.str_val_.data(), static_cast<int32_t>(cell.str_val_.size()));
      ObExprStrResAlloc res_alloc(*expr, eval_ctx_);
      if (OB_FAIL(datum.deep_copy(tmp, res_alloc))) {
      }
      break;
    }
    case FT_DATE: {
      if (OB_FAIL(set_date_datum(expr, cell))) {
      }
      break;
    }
    case FT_DATETIME: {
      if (OB_FAIL(set_datetime_datum(expr, cell))) {
      }
      break;
    }
    default: {
      ret = OB_INVALID_DATA;
      LOG_WARN("unexpected file cell type", K(cell.type_), K(ret));
      break;
    }
  }
  return ret;
}

int ObFileTableAccessOp::set_date_datum(ObExpr *expr, const FileCell &cell)
{
  int ret = OB_SUCCESS;
  std::string text = format_date_days(cell.date_days_);
  ObString str(static_cast<int32_t>(text.size()), text.data());
  int32_t date_value = 0;
  ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
  if (OB_FAIL(ObTimeConverter::str_to_date(str, date_value, date_sql_mode_))) {
    LOG_WARN("failed to convert date value", K(text.c_str()), K(ret));
    LOG_USER_ERROR(OB_INVALID_DATA);
  } else {
    datum.set_date(date_value);
  }
  return ret;
}

int ObFileTableAccessOp::set_datetime_datum(ObExpr *expr, const FileCell &cell)
{
  int ret = OB_SUCCESS;
  std::string text = format_datetime_us(cell.datetime_us_);
  ObString str(static_cast<int32_t>(text.size()), text.data());
  int64_t datetime_value = 0;
  ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
  ObTimeConvertCtx cvrt_ctx(GET_MY_SESSION(ctx_)->get_timezone_info(), false);
  if (OB_FAIL(ObTimeConverter::str_to_datetime(str, cvrt_ctx, datetime_value, NULL, date_sql_mode_))) {
    LOG_WARN("failed to convert datetime value", K(text.c_str()), K(ret));
    LOG_USER_ERROR(OB_INVALID_DATA);
  } else {
    datum.set_datetime(datetime_value);
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
