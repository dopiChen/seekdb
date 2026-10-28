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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_ACCESS_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_ACCESS_OP_H_

#include <string>
#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

class ObPhyOpSeriCtx;

/**
 * Physical spec of a file table access. The resolver has already inferred the
 * schema (and recorded the file signature) so the executor only needs to stream
 * the file and cast each cell into the output expressions.
 */
class ObFileTableAccessSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileTableAccessSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      path_(),
      format_(0),
      delimiter_(','),
      quote_('"'),
      escape_('"'),
      has_header_(true),
      null_str_(),
      row_count_(0),
      file_size_(0),
      file_mtime_sec_(0),
      file_mtime_nsec_(0),
      file_inode_(0),
      column_exprs_(alloc)
  {}
  INHERIT_TO_STRING_KV("op_spec", ObOpSpec, K_(path), K_(format), K_(delimiter),
                       K_(quote), K_(escape), K_(has_header), K_(row_count));
public:
  common::ObString path_;
  int32_t format_;
  int32_t delimiter_;
  int32_t quote_;
  int32_t escape_;
  bool has_header_;
  common::ObString null_str_;
  int64_t row_count_;
  int64_t file_size_;
  int64_t file_mtime_sec_;
  int64_t file_mtime_nsec_;
  int64_t file_inode_;
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObFileTableAccessSpec);
};

/**
 * Streaming file table access operator.
 */
class ObFileTableAccessOp : public ObOperator
{
public:
  ObFileTableAccessOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      reader_(NULL),
      row_(),
      date_sql_mode_(0),
      opened_(false)
  {}
  virtual ~ObFileTableAccessOp();
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int build_read_options(file_sql::FileReadOptions &options) const;
  int verify_file_signature();
  int open_reader();
  int close_reader();
  int fill_row(const file_sql::FileRow &row);
  int set_cell_datum(ObExpr *expr, const file_sql::FileCell &cell);
  int set_date_datum(ObExpr *expr, const file_sql::FileCell &cell);
  int set_datetime_datum(ObExpr *expr, const file_sql::FileCell &cell);
private:
  file_sql::FileScanReader *reader_;
  file_sql::FileRow row_;
  common::ObDateSqlMode date_sql_mode_;
  bool opened_;
  DISALLOW_COPY_AND_ASSIGN(ObFileTableAccessOp);
};

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_ACCESS_OP_H_
