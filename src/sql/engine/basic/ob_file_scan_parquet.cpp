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

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

namespace
{

void parquet_set_err(std::string &err, const char *fmt, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  err.assign(buf);
}

// ---------------------------------------------------------------------------
// thrift compact protocol constants
// ---------------------------------------------------------------------------
enum CompactType : int32_t
{
  CT_STOP = 0,
  CT_TRUE = 1,
  CT_FALSE = 2,
  CT_BYTE = 3,
  CT_I16 = 4,
  CT_I32 = 5,
  CT_I64 = 6,
  CT_DOUBLE = 7,
  CT_BINARY = 8,
  CT_LIST = 9,
  CT_SET = 10,
  CT_MAP = 11,
  CT_STRUCT = 12,
};

enum ParquetType : int32_t
{
  PT_BOOLEAN = 0,
  PT_INT32 = 1,
  PT_INT64 = 2,
  PT_INT96 = 3,
  PT_FLOAT = 4,
  PT_DOUBLE = 5,
  PT_BYTE_ARRAY = 6,
  PT_FIXED_LEN_BYTE_ARRAY = 7,
};

enum ParquetRepetition : int32_t
{
  PR_REQUIRED = 0,
  PR_OPTIONAL = 1,
  PR_REPEATED = 2,
};

enum ParquetEncoding : int32_t
{
  PE_PLAIN = 0,
  PE_PLAIN_DICTIONARY = 2,
  PE_RLE = 3,
  PE_BIT_PACKED = 4,
  PE_RLE_DICTIONARY = 8,
};

enum ParquetCodec : int32_t
{
  PC_UNCOMPRESSED = 0,
  PC_SNAPPY = 1,
  PC_GZIP = 2,
};

enum ParquetConvertedType : int32_t
{
  PCT_UTF8 = 0,
  PCT_MAP = 1,
  PCT_MAP_KEY_VALUE = 2,
  PCT_LIST = 3,
  PCT_ENUM = 4,
  PCT_DECIMAL = 5,
  PCT_DATE = 6,
  PCT_TIME_MILLIS = 7,
  PCT_TIME_MICROS = 8,
  PCT_TIMESTAMP_MILLIS = 9,
  PCT_TIMESTAMP_MICROS = 10,
  PCT_UINT_8 = 11,
  PCT_UINT_16 = 12,
  PCT_UINT_32 = 13,
  PCT_UINT_64 = 14,
  PCT_INT_8 = 15,
  PCT_INT_16 = 16,
  PCT_INT_32 = 17,
  PCT_INT_64 = 18,
  PCT_JSON = 19,
  PCT_BSON = 20,
  PCT_INTERVAL = 21,
};

// ---------------------------------------------------------------------------
// buffered byte reader
// ---------------------------------------------------------------------------
class ByteReader
{
public:
  ByteReader() : p_(NULL), end_(NULL) {}
  ByteReader(const uint8_t *data, size_t len) : p_(data), end_(data + len) {}
  void reset(const uint8_t *data, size_t len) { p_ = data; end_ = data + len; }
  bool empty() const { return p_ >= end_; }
  size_t remain() const { return static_cast<size_t>(end_ - p_); }
  const uint8_t *cur() const { return p_; }
  int skip(size_t n)
  {
    int ret = 0;
    if (remain() < n) {
      ret = FS_ERR_BAD_DATA;
    } else {
      p_ += n;
    }
    return ret;
  }
  int read_u8(uint8_t &v)
  {
    int ret = 0;
    if (remain() < 1) {
      ret = FS_ERR_BAD_DATA;
    } else {
      v = *p_++;
    }
    return ret;
  }
  int read_le32(uint32_t &v)
  {
    int ret = 0;
    if (remain() < 4) {
      ret = FS_ERR_BAD_DATA;
    } else {
      v = static_cast<uint32_t>(p_[0]) | (static_cast<uint32_t>(p_[1]) << 8) |
          (static_cast<uint32_t>(p_[2]) << 16) | (static_cast<uint32_t>(p_[3]) << 24);
      p_ += 4;
    }
    return ret;
  }
  int read_le64(uint64_t &v)
  {
    int ret = 0;
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (FS_OK == (ret = read_le32(lo)) && FS_OK == (ret = read_le32(hi))) {
      v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return ret;
  }
  int read_double(double &v)
  {
    uint64_t bits = 0;
    int ret = read_le64(bits);
    if (FS_OK == ret) {
      memcpy(&v, &bits, sizeof(v));
    }
    return ret;
  }
  int read_bytes(std::string &s, uint32_t len)
  {
    int ret = 0;
    if (remain() < len) {
      ret = FS_ERR_BAD_DATA;
    } else {
      s.assign(reinterpret_cast<const char *>(p_), len);
      p_ += len;
    }
    return ret;
  }
private:
  const uint8_t *p_;
  const uint8_t *end_;
};

// ---------------------------------------------------------------------------
// thrift compact protocol reader
// ---------------------------------------------------------------------------
class CompactReader
{
public:
  CompactReader() : last_field_(0) {}
  void reset(const uint8_t *data, size_t len)
  {
    br_.reset(data, len);
    last_field_ = 0;
  }
  size_t remain() const { return br_.remain(); }
  // Thrift compact protocol tracks the last field id per struct.  Nested
  // structs have their own field id sequence, so callers must save and reset
  // the state before parsing a nested struct and restore it afterwards.
  int16_t enter_struct()
  {
    const int16_t saved = last_field_;
    last_field_ = 0;
    return saved;
  }
  void leave_struct(int16_t saved) { last_field_ = saved; }
  // returns 1 on STOP, 0 on success, negative on error
  int read_field_header(int &field_id, int8_t &type)
  {
    int ret = 0;
    uint8_t b = 0;
    if (FS_OK != (ret = br_.read_u8(b))) {
    } else if (CT_STOP == (b & 0x0F)) {
      ret = 1;
    } else {
      type = static_cast<int8_t>(b & 0x0F);
      const int16_t delta = static_cast<int16_t>(b >> 4);
      if (0 != delta) {
        field_id = static_cast<int>(last_field_) + delta;
      } else {
        int32_t id = 0;
        if (FS_OK != (ret = read_i16_raw(id))) {
        } else {
          field_id = id;
        }
      }
      last_field_ = static_cast<int16_t>(field_id);
    }
    return ret;
  }
  int read_varint(uint64_t &value)
  {
    int ret = 0;
    uint64_t result = 0;
    int shift = 0;
    uint8_t b = 0;
    bool done = false;
    while (0 == ret && !done) {
      if (shift > 63) {
        ret = FS_ERR_BAD_DATA;
      } else if (FS_OK != (ret = br_.read_u8(b))) {
      } else {
        result |= (static_cast<uint64_t>(b & 0x7F) << shift);
        if (0 == (b & 0x80)) {
          done = true;
        } else {
          shift += 7;
        }
      }
    }
    value = result;
    return ret;
  }
  int read_zigzag(int64_t &value)
  {
    int ret = 0;
    uint64_t raw = 0;
    if (FS_OK != (ret = read_varint(raw))) {
    } else {
      value = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }
    return ret;
  }
  int read_i16_raw(int32_t &value)
  {
    int ret = 0;
    int64_t v = 0;
    if (FS_OK != (ret = read_zigzag(v))) {
    } else {
      value = static_cast<int32_t>(v);
    }
    return ret;
  }
  int read_i32(int32_t &value)
  {
    int ret = 0;
    int64_t v = 0;
    if (FS_OK != (ret = read_zigzag(v))) {
    } else {
      value = static_cast<int32_t>(v);
    }
    return ret;
  }
  int read_i64(int64_t &value) { return read_zigzag(value); }
  int read_bool(int8_t type, bool &value)
  {
    int ret = 0;
    if (CT_TRUE == type) {
      value = true;
    } else if (CT_FALSE == type) {
      value = false;
    } else {
      uint8_t b = 0;
      if (FS_OK != (ret = br_.read_u8(b))) {
      } else {
        value = (1 == b);
      }
    }
    return ret;
  }
  int read_double(double &value)
  {
    int ret = 0;
    uint64_t raw = 0;
    if (FS_OK != (ret = br_.read_le64(raw))) {
    } else {
      memcpy(&value, &raw, sizeof(value));
    }
    return ret;
  }
  int read_binary(std::string &value)
  {
    int ret = 0;
    uint64_t len = 0;
    if (FS_OK != (ret = read_varint(len)) || len > static_cast<uint64_t>(br_.remain())) {
      ret = FS_ERR_BAD_DATA;
    } else {
      ret = br_.read_bytes(value, static_cast<uint32_t>(len));
    }
    return ret;
  }
  int read_list_header(int8_t &elem_type, uint32_t &size)
  {
    int ret = 0;
    uint8_t b = 0;
    if (FS_OK != (ret = br_.read_u8(b))) {
    } else {
      elem_type = static_cast<int8_t>(b & 0x0F);
      size = static_cast<uint32_t>(b >> 4);
      if (15 == size) {
        uint64_t v = 0;
        if (FS_OK != (ret = read_varint(v)) || v > 0x7FFFFFFFULL) {
          ret = FS_ERR_BAD_DATA;
        } else {
          size = static_cast<uint32_t>(v);
        }
      }
    }
    return ret;
  }
  int skip(int8_t type)
  {
    int ret = 0;
    switch (type) {
      case CT_TRUE:
      case CT_FALSE:
        break;
      case CT_BYTE: {
        uint8_t b = 0;
        ret = br_.read_u8(b);
        break;
      }
      case CT_I16:
      case CT_I32:
      case CT_I64: {
        int64_t v = 0;
        ret = read_zigzag(v);
        break;
      }
      case CT_DOUBLE: {
        double v = 0.0;
        ret = read_double(v);
        break;
      }
      case CT_BINARY: {
        std::string s;
        ret = read_binary(s);
        break;
      }
      case CT_LIST:
      case CT_SET: {
        int8_t elem_type = 0;
        uint32_t size = 0;
        if (FS_OK != (ret = read_list_header(elem_type, size))) {
        } else {
          for (uint32_t i = 0; FS_OK == ret && i < size; ++i) {
            ret = skip(elem_type);
          }
        }
        break;
      }
      case CT_MAP: {
        uint8_t b = 0;
        if (FS_OK != (ret = br_.read_u8(b))) {
        } else {
          uint32_t size = static_cast<uint32_t>(b >> 4);
          const int8_t key_type = static_cast<int8_t>(b & 0x0F);
          int8_t val_type = 0;
          if (15 == size) {
            uint64_t v = 0;
            if (FS_OK != (ret = read_varint(v))) {
            } else {
              size = static_cast<uint32_t>(v);
            }
          }
          if (FS_OK == ret && size > 0) {
            uint8_t vb = 0;
            if (FS_OK != (ret = br_.read_u8(vb))) {
            } else {
              val_type = static_cast<int8_t>(vb);
            }
          }
          for (uint32_t i = 0; FS_OK == ret && i < size; ++i) {
            ret = skip(key_type);
            if (FS_OK == ret) {
              ret = skip(val_type);
            }
          }
        }
        break;
      }
      case CT_STRUCT: {
        const int16_t saved_field = enter_struct();
        bool done = false;
        while (FS_OK == ret && !done) {
          int field_id = 0;
          int8_t field_type = 0;
          const int rc = read_field_header(field_id, field_type);
          if (1 == rc) {
            done = true;
          } else if (rc < 0) {
            ret = FS_ERR_BAD_DATA;
          } else {
            ret = skip(field_type);
          }
        }
        leave_struct(saved_field);
        break;
      }
      default:
        ret = FS_ERR_BAD_DATA;
        break;
    }
    return ret;
  }
private:
  ByteReader br_;
  int16_t last_field_;
};

// ---------------------------------------------------------------------------
// parquet metadata structures
// ---------------------------------------------------------------------------
struct SchemaElement
{
  int32_t type_ = -1;
  int32_t type_length_ = -1;
  int32_t repetition_type_ = -1;
  std::string name_;
  int32_t num_children_ = -1;
  int32_t converted_type_ = -1;
  int32_t scale_ = -1;
  int32_t precision_ = -1;
};

struct ColumnMetaData
{
  int32_t type_ = -1;
  std::vector<int32_t> encodings_;
  std::vector<std::string> path_in_schema_;
  int32_t codec_ = 0;
  int64_t num_values_ = 0;
  int64_t total_uncompressed_size_ = 0;
  int64_t total_compressed_size_ = 0;
  int64_t data_page_offset_ = 0;
  int64_t index_page_offset_ = 0;
  int64_t dictionary_page_offset_ = 0;
};

struct ColumnChunk
{
  std::string file_path_;
  int64_t file_offset_ = 0;
  ColumnMetaData meta_;
  bool has_meta_ = false;
};

struct RowGroup
{
  std::vector<ColumnChunk> columns_;
  int64_t total_byte_size_ = 0;
  int64_t num_rows_ = 0;
};

struct FileMetaData
{
  int32_t version_ = 0;
  std::vector<SchemaElement> schema_;
  int64_t num_rows_ = 0;
  std::vector<RowGroup> row_groups_;
};

struct PageHeader
{
  int32_t type_ = -1;
  int32_t uncompressed_page_size_ = 0;
  int32_t compressed_page_size_ = 0;
  int32_t crc_ = 0;
  int32_t num_values_ = 0;
  int32_t encoding_ = -1;
  int32_t def_level_encoding_ = -1;
  int32_t rep_level_encoding_ = -1;
  int32_t dict_num_values_ = 0;
  int32_t dict_encoding_ = -1;
  int32_t v2_num_values_ = 0;
  int32_t v2_num_nulls_ = 0;
  int32_t v2_num_rows_ = 0;
  int32_t v2_encoding_ = -1;
  int32_t v2_def_len_ = 0;
  int32_t v2_rep_len_ = 0;
  bool v2_is_compressed_ = true;
};

int parse_schema_element(CompactReader &cr, SchemaElement &e)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: ret = cr.read_i32(e.type_); break;
        case 2: ret = cr.read_i32(e.type_length_); break;
        case 3: ret = cr.read_i32(e.repetition_type_); break;
        case 4: ret = cr.read_binary(e.name_); break;
        case 5: ret = cr.read_i32(e.num_children_); break;
        case 6: ret = cr.read_i32(e.converted_type_); break;
        case 7: ret = cr.read_i32(e.scale_); break;
        case 8: ret = cr.read_i32(e.precision_); break;
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

int parse_column_meta(CompactReader &cr, ColumnMetaData &m)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: ret = cr.read_i32(m.type_); break;
        case 2: {
          int8_t elem_type = 0;
          uint32_t size = 0;
          if (FS_OK != (ret = cr.read_list_header(elem_type, size))) {
          } else {
            m.encodings_.reserve(size);
            for (uint32_t i = 0; FS_OK == ret && i < size; ++i) {
              int32_t v = 0;
              if (FS_OK != (ret = cr.read_i32(v))) {
              } else {
                m.encodings_.push_back(v);
              }
            }
          }
          break;
        }
        case 3: {
          int8_t elem_type = 0;
          uint32_t size = 0;
          if (FS_OK != (ret = cr.read_list_header(elem_type, size))) {
          } else {
            m.path_in_schema_.reserve(size);
            for (uint32_t i = 0; FS_OK == ret && i < size; ++i) {
              std::string s;
              if (FS_OK != (ret = cr.read_binary(s))) {
              } else {
                m.path_in_schema_.push_back(s);
              }
            }
          }
          break;
        }
        case 4: ret = cr.read_i32(m.codec_); break;
        case 5: ret = cr.read_i64(m.num_values_); break;
        case 6: ret = cr.read_i64(m.total_uncompressed_size_); break;
        case 7: ret = cr.read_i64(m.total_compressed_size_); break;
        case 8: ret = cr.read_i64(m.data_page_offset_); break;
        case 9: ret = cr.read_i64(m.index_page_offset_); break;
        case 10: ret = cr.read_i64(m.dictionary_page_offset_); break;
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

int parse_column_chunk(CompactReader &cr, ColumnChunk &c)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: ret = cr.read_binary(c.file_path_); break;
        case 2: ret = cr.read_i64(c.file_offset_); break;
        case 3:
          if (CT_STRUCT != type) {
            ret = cr.skip(type);
          } else {
            ret = parse_column_meta(cr, c.meta_);
            c.has_meta_ = (FS_OK == ret);
          }
          break;
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

int parse_row_group(CompactReader &cr, RowGroup &rg)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: {
          int8_t elem_type = 0;
          uint32_t size = 0;
          if (FS_OK != (ret = cr.read_list_header(elem_type, size))) {
          } else {
            rg.columns_.resize(size);
            for (uint32_t i = 0; FS_OK == ret && i < size; ++i) {
              ret = parse_column_chunk(cr, rg.columns_[i]);
            }
          }
          break;
        }
        case 2: ret = cr.read_i64(rg.total_byte_size_); break;
        case 3: ret = cr.read_i64(rg.num_rows_); break;
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

int parse_file_metadata(CompactReader &cr, FileMetaData &meta)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: ret = cr.read_i32(meta.version_); break;
        case 2: {
          int8_t elem_type = 0;
          uint32_t size = 0;
          if (FS_OK != (ret = cr.read_list_header(elem_type, size))) {
          } else if (size > 1000000) {
            ret = FS_ERR_BAD_DATA;
          } else {
            meta.schema_.resize(size);
            for (uint32_t i = 0; FS_OK == ret && i < size; ++i) {
              ret = parse_schema_element(cr, meta.schema_[i]);
            }
          }
          break;
        }
        case 3: ret = cr.read_i64(meta.num_rows_); break;
        case 4: {
          int8_t elem_type = 0;
          uint32_t size = 0;
          if (FS_OK != (ret = cr.read_list_header(elem_type, size))) {
          } else if (size > 10000000) {
            ret = FS_ERR_BAD_DATA;
          } else {
            meta.row_groups_.resize(size);
            for (uint32_t i = 0; FS_OK == ret && i < size; ++i) {
              ret = parse_row_group(cr, meta.row_groups_[i]);
            }
          }
          break;
        }
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

int parse_data_page_header(CompactReader &cr, PageHeader &h)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: ret = cr.read_i32(h.num_values_); break;
        case 2: ret = cr.read_i32(h.encoding_); break;
        case 3: ret = cr.read_i32(h.def_level_encoding_); break;
        case 4: ret = cr.read_i32(h.rep_level_encoding_); break;
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

int parse_dictionary_page_header(CompactReader &cr, PageHeader &h)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: ret = cr.read_i32(h.dict_num_values_); break;
        case 2: ret = cr.read_i32(h.dict_encoding_); break;
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

int parse_data_page_header_v2(CompactReader &cr, PageHeader &h)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: ret = cr.read_i32(h.v2_num_values_); break;
        case 2: ret = cr.read_i32(h.v2_num_nulls_); break;
        case 3: ret = cr.read_i32(h.v2_num_rows_); break;
        case 4: ret = cr.read_i32(h.v2_encoding_); break;
        case 5: ret = cr.read_i32(h.v2_def_len_); break;
        case 6: ret = cr.read_i32(h.v2_rep_len_); break;
        case 7: ret = cr.read_bool(type, h.v2_is_compressed_); break;
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

int parse_page_header(CompactReader &cr, PageHeader &h)
{
  int ret = FS_OK;
  const int16_t saved_field = cr.enter_struct();
  bool done = false;
  while (FS_OK == ret && !done) {
    int field_id = 0;
    int8_t type = 0;
    const int rc = cr.read_field_header(field_id, type);
    if (1 == rc) {
      done = true;
    } else if (rc < 0) {
      ret = FS_ERR_BAD_DATA;
    } else {
      switch (field_id) {
        case 1: ret = cr.read_i32(h.type_); break;
        case 2: ret = cr.read_i32(h.uncompressed_page_size_); break;
        case 3: ret = cr.read_i32(h.compressed_page_size_); break;
        case 4: ret = cr.read_i32(h.crc_); break;
        case 5:
          if (CT_STRUCT != type) {
            ret = cr.skip(type);
          } else {
            ret = parse_data_page_header(cr, h);
          }
          break;
        case 7:
          if (CT_STRUCT != type) {
            ret = cr.skip(type);
          } else {
            ret = parse_dictionary_page_header(cr, h);
          }
          break;
        case 8:
          if (CT_STRUCT != type) {
            ret = cr.skip(type);
          } else {
            ret = parse_data_page_header_v2(cr, h);
          }
          break;
        default: ret = cr.skip(type); break;
      }
    }
  }
  cr.leave_struct(saved_field);
  return ret;
}

// ---------------------------------------------------------------------------
// snappy
// ---------------------------------------------------------------------------
int snappy_uncompress(const uint8_t *src, size_t src_len,
                      std::vector<uint8_t> &dst, std::string &err)
{
  int ret = FS_OK;
  size_t pos = 0;
  uint64_t uncompressed_len = 0;
  int shift = 0;
  bool len_done = false;
  while (FS_OK == ret && !len_done) {
    if (pos >= src_len || shift > 63) {
      ret = FS_ERR_BAD_DATA;
    } else {
      const uint8_t b = src[pos++];
      uncompressed_len |= (static_cast<uint64_t>(b & 0x7F) << shift);
      if (0 == (b & 0x80)) {
        len_done = true;
      } else {
        shift += 7;
      }
    }
  }
  if (FS_OK == ret && uncompressed_len > (256ULL << 20)) {
    ret = FS_ERR_BAD_DATA;
  }
  if (FS_OK == ret) {
    dst.resize(static_cast<size_t>(uncompressed_len));
  }
  size_t out = 0;
  while (FS_OK == ret && pos < src_len) {
    const uint8_t tag = src[pos++];
    const int kind = tag & 0x03;
    if (0 == kind) {
      size_t len = static_cast<size_t>(tag >> 2) + 1;
      if (len > 60) {
        const size_t extra = len - 60;
        if (pos + extra > src_len) {
          ret = FS_ERR_BAD_DATA;
        } else {
          uint64_t v = 0;
          for (size_t i = 0; i < extra; ++i) {
            v |= (static_cast<uint64_t>(src[pos + i]) << (8 * i));
          }
          pos += extra;
          len = static_cast<size_t>(v) + 1;
        }
      }
      if (FS_OK == ret) {
        if (pos + len > src_len || out + len > dst.size()) {
          ret = FS_ERR_BAD_DATA;
        } else {
          memcpy(dst.data() + out, src + pos, len);
          pos += len;
          out += len;
        }
      }
    } else {
      size_t len = 0;
      size_t offset = 0;
      if (1 == kind) {
        if (pos >= src_len) {
          ret = FS_ERR_BAD_DATA;
        } else {
          len = ((tag >> 2) & 0x07) + 4;
          offset = (static_cast<size_t>(tag >> 5) << 8) | src[pos++];
        }
      } else if (2 == kind) {
        if (pos + 2 > src_len) {
          ret = FS_ERR_BAD_DATA;
        } else {
          len = static_cast<size_t>(tag >> 2) + 1;
          offset = static_cast<size_t>(src[pos]) | (static_cast<size_t>(src[pos + 1]) << 8);
          pos += 2;
        }
      } else {
        if (pos + 4 > src_len) {
          ret = FS_ERR_BAD_DATA;
        } else {
          len = static_cast<size_t>(tag >> 2) + 1;
          offset = static_cast<size_t>(src[pos]) | (static_cast<size_t>(src[pos + 1]) << 8) |
                   (static_cast<size_t>(src[pos + 2]) << 16) |
                   (static_cast<size_t>(src[pos + 3]) << 24);
          pos += 4;
        }
      }
      if (FS_OK == ret) {
        if (0 == offset || offset > out || out + len > dst.size()) {
          ret = FS_ERR_BAD_DATA;
        } else {
          for (size_t i = 0; i < len; ++i) {
            dst[out + i] = dst[out - offset + i];
          }
          out += len;
        }
      }
    }
  }
  if (FS_OK == ret && out != dst.size()) {
    ret = FS_ERR_BAD_DATA;
  }
  if (FS_OK != ret) {
    parquet_set_err(err, "snappy decompress failed");
  }
  return ret;
}

// ---------------------------------------------------------------------------
// RLE / bit-packed hybrid
// ---------------------------------------------------------------------------
uint32_t read_bits(const uint8_t *data, size_t bit_offset, int bit_width)
{
  uint32_t value = 0;
  for (int i = 0; i < bit_width; ++i) {
    const size_t off = bit_offset + static_cast<size_t>(i);
    const uint32_t bit = (data[off >> 3] >> (off & 7)) & 1;
    value |= (bit << i);
  }
  return value;
}

int rle_decode(const uint8_t *data, size_t len, int bit_width, int64_t count,
               std::vector<uint32_t> &out)
{
  int ret = FS_OK;
  out.clear();
  size_t pos = 0;
  if (bit_width < 0 || bit_width > 32) {
    ret = FS_ERR_BAD_DATA;
  }
  while (FS_OK == ret && static_cast<int64_t>(out.size()) < count) {
    if (pos >= len) {
      ret = FS_ERR_BAD_DATA;
    } else {
      uint64_t header = 0;
      int shift = 0;
      bool done = false;
      while (FS_OK == ret && !done) {
        if (pos >= len || shift > 63) {
          ret = FS_ERR_BAD_DATA;
        } else {
          const uint8_t b = data[pos++];
          header |= (static_cast<uint64_t>(b & 0x7F) << shift);
          if (0 == (b & 0x80)) {
            done = true;
          } else {
            shift += 7;
          }
        }
      }
      if (FS_OK == ret) {
        if (header & 1) {
          const uint64_t groups = header >> 1;
          const size_t bytes = static_cast<size_t>(groups) * static_cast<size_t>(bit_width);
          if (groups > (1ULL << 24) || pos + bytes > len) {
            ret = FS_ERR_BAD_DATA;
          } else {
            const size_t total = static_cast<size_t>(groups) * 8;
            for (size_t i = 0; FS_OK == ret && i < total &&
                                static_cast<int64_t>(out.size()) < count; ++i) {
              out.push_back(bit_width > 0 ? read_bits(data + pos, i * bit_width, bit_width) : 0);
            }
            pos += bytes;
          }
        } else {
          const uint64_t run = header >> 1;
          const size_t byte_w = static_cast<size_t>(bit_width + 7) / 8;
          if (run > (1ULL << 32) || pos + byte_w > len) {
            ret = FS_ERR_BAD_DATA;
          } else {
            uint32_t value = 0;
            for (size_t i = 0; i < byte_w; ++i) {
              value |= (static_cast<uint32_t>(data[pos + i]) << (8 * i));
            }
            pos += byte_w;
            for (uint64_t i = 0; FS_OK == ret && i < run &&
                                 static_cast<int64_t>(out.size()) < count; ++i) {
              out.push_back(value);
            }
          }
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// parquet reader
// ---------------------------------------------------------------------------
struct LeafColumn
{
  int32_t physical_type_ = -1;
  int32_t converted_type_ = -1;
  int32_t scale_ = -1;
  int32_t repetition_type_ = -1;
  bool nested_ = false;
  int64_t file_column_ = -1;  // index in the exposed schema, -1 for nested leaves
};

class ParquetFileReader : public FileScanReader
{
public:
  ParquetFileReader(const FileReadOptions &options, const std::string &path)
    : options_(options)
  {
    path_ = path;
  }
  virtual ~ParquetFileReader() { close(); }
  virtual int infer_schema(std::vector<FileColumnSchema> &schema, std::string &err) override;
  virtual int open(std::string &err) override;
  virtual int next(FileRow &row, bool &eof, std::string &err) override;
  virtual void close() override;
  virtual int64_t estimated_rows() const override { return total_rows_; }
  void set_needed_columns(const std::vector<bool> &needed) { needed_ = needed; }
private:
  int load_footer(std::string &err);
  int build_schema(std::string &err);
  int read_page_header_at(int64_t offset, PageHeader &header, int64_t &header_size,
                          std::string &err);
  int load_row_group(int64_t rg_idx, std::string &err);
  int decode_column(const ColumnMetaData &meta, int64_t leaf_idx, int64_t num_rows,
                    std::vector<FileCell> &cells, std::string &err);
  int convert_value(int32_t physical_type, int32_t converted_type, int32_t scale,
                    uint32_t bits, int64_t int_value, double double_value,
                    const std::string &bytes, FileCell &cell, std::string &err);
  FileReadOptions options_;
  int fd_ = -1;
  FileSignature signature_;
  FileMetaData meta_;
  bool footer_loaded_ = false;
  std::vector<FileColumnSchema> schema_;
  std::vector<LeafColumn> leaves_;
  std::vector<bool> needed_;
  int64_t total_rows_ = 0;
  int64_t current_rg_ = 0;
  int64_t row_in_rg_ = 0;
  std::vector<std::vector<FileCell> > rg_cells_;
  std::vector<std::string> rg_err_;
};

int ParquetFileReader::read_page_header_at(int64_t offset, PageHeader &header,
                                           int64_t &header_size, std::string &err)
{
  int ret = FS_OK;
  header_size = 0;
  const size_t kReadSize = 256 * 1024;
  std::vector<uint8_t> buf(kReadSize);
  const ssize_t n = pread(fd_, buf.data(), kReadSize, offset);
  if (n < 0) {
    ret = FS_ERR_IO;
    parquet_set_err(err, "read '%s' failed: %s", path_.c_str(), strerror(errno));
  } else if (0 == n) {
    ret = FS_ERR_BAD_DATA;
    parquet_set_err(err, "unexpected end of parquet file '%s'", path_.c_str());
  } else {
    CompactReader cr;
    cr.reset(buf.data(), static_cast<size_t>(n));
    if (FS_OK != (ret = parse_page_header(cr, header))) {
      parquet_set_err(err, "%s: corrupted page header", path_.c_str());
    } else {
      header_size = static_cast<int64_t>(n) - static_cast<int64_t>(cr.remain());
    }
  }
  return ret;
}

int ParquetFileReader::load_footer(std::string &err)
{
  int ret = FS_OK;
  footer_loaded_ = false;
  meta_ = FileMetaData();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if ((fd_ = ::open(path_.c_str(), O_RDONLY)) < 0) {
    ret = ENOENT == errno ? FS_ERR_NOT_EXIST : FS_ERR_IO;
    parquet_set_err(err, "open '%s' failed: %s", path_.c_str(), strerror(errno));
  } else if (FS_OK != (ret = stat_file_signature(path_, signature_, err))) {
  } else if (signature_.size_ < 12) {
    ret = FS_ERR_BAD_DATA;
    parquet_set_err(err, "%s: file is too small to be a parquet file", path_.c_str());
  } else {
    uint8_t tail[8];
    if (8 != pread(fd_, tail, 8, signature_.size_ - 8)) {
      ret = FS_ERR_IO;
      parquet_set_err(err, "read footer of '%s' failed", path_.c_str());
    } else if (0 != memcmp(tail + 4, "PAR1", 4)) {
      ret = FS_ERR_BAD_DATA;
      parquet_set_err(err, "%s: invalid parquet magic", path_.c_str());
    } else {
      const uint32_t footer_len = static_cast<uint32_t>(tail[0]) |
                                  (static_cast<uint32_t>(tail[1]) << 8) |
                                  (static_cast<uint32_t>(tail[2]) << 16) |
                                  (static_cast<uint32_t>(tail[3]) << 24);
      const int64_t footer_offset = signature_.size_ - 8 - static_cast<int64_t>(footer_len);
      if (footer_offset < 4 || footer_len > (256U << 20)) {
        ret = FS_ERR_BAD_DATA;
        parquet_set_err(err, "%s: invalid parquet footer length", path_.c_str());
      } else {
        uint8_t head[4];
        if (4 != pread(fd_, head, 4, 0) || 0 != memcmp(head, "PAR1", 4)) {
          ret = FS_ERR_BAD_DATA;
          parquet_set_err(err, "%s: invalid parquet header magic", path_.c_str());
        } else {
          std::vector<uint8_t> footer(footer_len);
          if (static_cast<ssize_t>(footer_len) != pread(fd_, footer.data(), footer_len,
                                                        footer_offset)) {
            ret = FS_ERR_IO;
            parquet_set_err(err, "read footer of '%s' failed", path_.c_str());
          } else {
            CompactReader cr;
            cr.reset(footer.data(), footer.size());
            if (FS_OK != (ret = parse_file_metadata(cr, meta_))) {
              parquet_set_err(err, "%s: cannot parse parquet footer", path_.c_str());
            } else {
              footer_loaded_ = true;
              total_rows_ = meta_.num_rows_;
            }
          }
        }
      }
    }
  }
  return ret;
}

int ParquetFileReader::build_schema(std::string &err)
{
  int ret = FS_OK;
  schema_.clear();
  leaves_.clear();
  if (meta_.schema_.empty()) {
    ret = FS_ERR_BAD_DATA;
    parquet_set_err(err, "%s: empty parquet schema", path_.c_str());
  } else {
    const SchemaElement &root = meta_.schema_[0];
    const int64_t child_cnt = root.num_children_ < 0 ? 0 : root.num_children_;
    int64_t idx = 1;
    for (int64_t child = 0; FS_OK == ret && child < child_cnt && idx < static_cast<int64_t>(meta_.schema_.size());
         ++child) {
      const SchemaElement &elem = meta_.schema_[idx];
      if (elem.num_children_ > 0) {
        // nested column: keep the leaf elements so that the column order of
        // row groups stays aligned, but never expose it to SQL.
        FileColumnSchema col;
        col.name_ = elem.name_;
        col.type_ = FT_VARCHAR;
        col.nullable_ = true;
        col.parquet_type_ = -1;
        col.parquet_converted_type_ = -1;
        schema_.push_back(col);
        int64_t sub = idx + 1;
        const int64_t sub_end = sub + elem.num_children_;
        for (; FS_OK == ret && sub < sub_end && sub < static_cast<int64_t>(meta_.schema_.size()); ++sub) {
          LeafColumn leaf;
          const SchemaElement &sub_elem = meta_.schema_[sub];
          leaf.physical_type_ = sub_elem.type_;
          leaf.converted_type_ = sub_elem.converted_type_;
          leaf.scale_ = sub_elem.scale_;
          leaf.repetition_type_ = sub_elem.repetition_type_;
          leaf.nested_ = true;
          leaf.file_column_ = -1;
          leaves_.push_back(leaf);
          if (sub_elem.num_children_ > 0) {
            ret = FS_ERR_UNSUPPORTED;
            parquet_set_err(err, "%s: nested parquet schema is not supported", path_.c_str());
          }
        }
        idx = sub_end;
      } else {
        FileColumnSchema col;
        col.name_ = elem.name_;
        col.nullable_ = (PR_OPTIONAL == elem.repetition_type_ || -1 == elem.repetition_type_);
        col.parquet_type_ = elem.type_;
        col.parquet_converted_type_ = elem.converted_type_;
        switch (elem.type_) {
          case PT_BOOLEAN:
            col.type_ = FT_BOOLEAN;
            break;
          case PT_INT32:
            if (PCT_DATE == elem.converted_type_) {
              col.type_ = FT_DATE;
            } else {
              col.type_ = FT_BIGINT;
            }
            break;
          case PT_INT64:
            if (PCT_TIMESTAMP_MILLIS == elem.converted_type_ ||
                PCT_TIMESTAMP_MICROS == elem.converted_type_) {
              col.type_ = FT_DATETIME;
            } else {
              col.type_ = FT_BIGINT;
            }
            break;
          case PT_INT96:
            col.type_ = FT_DATETIME;
            break;
          case PT_FLOAT:
          case PT_DOUBLE:
            col.type_ = FT_DOUBLE;
            break;
          case PT_BYTE_ARRAY:
          case PT_FIXED_LEN_BYTE_ARRAY:
            col.type_ = FT_VARCHAR;
            break;
          default:
            col.type_ = FT_VARCHAR;
            break;
        }
        LeafColumn leaf;
        leaf.physical_type_ = elem.type_;
        leaf.converted_type_ = elem.converted_type_;
        leaf.scale_ = elem.scale_;
        leaf.repetition_type_ = elem.repetition_type_;
        leaf.nested_ = false;
        leaf.file_column_ = static_cast<int64_t>(schema_.size());
        leaves_.push_back(leaf);
        schema_.push_back(col);
        ++idx;
      }
    }
  }
  return ret;
}

int ParquetFileReader::infer_schema(std::vector<FileColumnSchema> &schema, std::string &err)
{
  int ret = FS_OK;
  if (FS_OK != (ret = load_footer(err))) {
  } else if (FS_OK != (ret = build_schema(err))) {
  } else {
    schema = schema_;
  }
  return ret;
}

int ParquetFileReader::open(std::string &err)
{
  int ret = FS_OK;
  if (!footer_loaded_) {
    ret = load_footer(err);
  }
  if (FS_OK == ret) {
    // detect file changes between schema inference and scan
    FileSignature current;
    if (FS_OK != (ret = stat_file_signature(path_, current, err))) {
    } else if (current != signature_) {
      ret = FS_ERR_FILE_CHANGED;
      parquet_set_err(err, "%s: file changed after schema inference", path_.c_str());
    } else if (needed_.size() != schema_.size()) {
      needed_.assign(schema_.size(), true);
    }
  }
  if (FS_OK == ret) {
    current_rg_ = 0;
    row_in_rg_ = 0;
    rg_cells_.clear();
  }
  return ret;
}

void ParquetFileReader::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  rg_cells_.clear();
  current_rg_ = 0;
  row_in_rg_ = 0;
}

int ParquetFileReader::convert_value(int32_t physical_type, int32_t converted_type,
                                     int32_t scale, uint32_t bits, int64_t int_value,
                                     double double_value, const std::string &bytes,
                                     FileCell &cell, std::string &err)
{
  int ret = FS_OK;
  cell.reset();
  if (PT_BOOLEAN == physical_type) {
    cell.type_ = FT_BOOLEAN;
    cell.bool_val_ = (0 != bits);
  } else if (PT_INT32 == physical_type || PT_INT64 == physical_type) {
    if (PCT_DATE == converted_type) {
      cell.type_ = FT_DATE;
      cell.date_days_ = static_cast<int32_t>(int_value);
    } else if (PCT_TIMESTAMP_MILLIS == converted_type) {
      cell.type_ = FT_DATETIME;
      cell.datetime_us_ = int_value * 1000LL;
    } else if (PCT_TIMESTAMP_MICROS == converted_type ||
               PCT_TIME_MICROS == converted_type) {
      cell.type_ = FT_DATETIME;
      cell.datetime_us_ = int_value;
    } else if (PCT_DECIMAL == converted_type && scale >= 0 && scale <= 18) {
      int64_t divisor = 1;
      for (int32_t i = 0; i < scale; ++i) {
        divisor *= 10;
      }
      cell.type_ = FT_DOUBLE;
      cell.double_val_ = static_cast<double>(int_value) / static_cast<double>(divisor);
    } else {
      cell.type_ = FT_BIGINT;
      cell.int_val_ = int_value;
    }
  } else if (PT_FLOAT == physical_type || PT_DOUBLE == physical_type) {
    cell.type_ = FT_DOUBLE;
    cell.double_val_ = double_value;
  } else if (PT_BYTE_ARRAY == physical_type || PT_FIXED_LEN_BYTE_ARRAY == physical_type) {
    if (PCT_DECIMAL == converted_type) {
      ret = FS_ERR_UNSUPPORTED;
      parquet_set_err(err, "binary decimal parquet column is not supported");
    } else {
      cell.type_ = FT_VARCHAR;
      cell.str_val_ = bytes;
    }
  } else {
    ret = FS_ERR_UNSUPPORTED;
    parquet_set_err(err, "unsupported parquet physical type %d", physical_type);
  }
  return ret;
}

int ParquetFileReader::decode_column(const ColumnMetaData &meta, int64_t leaf_idx,
                                     int64_t num_rows, std::vector<FileCell> &cells,
                                     std::string &err)
{
  int ret = FS_OK;
  cells.assign(static_cast<size_t>(num_rows), FileCell());
  if (leaf_idx < 0 || leaf_idx >= static_cast<int64_t>(leaves_.size())) {
    ret = FS_ERR_BAD_DATA;
    parquet_set_err(err, "%s: invalid leaf column", path_.c_str());
  } else if (meta.codec_ != PC_UNCOMPRESSED && meta.codec_ != PC_SNAPPY) {
    ret = FS_ERR_UNSUPPORTED;
    parquet_set_err(err, "%s: parquet compression codec %d is not supported",
            path_.c_str(), meta.codec_);
  } else if (meta.num_values_ < 0 || meta.num_values_ > (1LL << 40)) {
    ret = FS_ERR_BAD_DATA;
    parquet_set_err(err, "%s: invalid parquet column value count", path_.c_str());
  } else {
    const LeafColumn &leaf = leaves_[static_cast<size_t>(leaf_idx)];
    const bool optional = (PR_OPTIONAL == leaf.repetition_type_);
    int64_t offset = meta.dictionary_page_offset_ > 0 ? meta.dictionary_page_offset_
                                                      : meta.data_page_offset_;
    int64_t produced = 0;
    std::vector<FileCell> dict_values;
    bool has_dict = false;
    std::vector<uint8_t> page_buf;
    while (FS_OK == ret && produced < meta.num_values_ && produced < num_rows) {
      PageHeader header;
      int64_t header_size = 0;
      if (FS_OK != (ret = read_page_header_at(offset, header, header_size, err))) {
      } else if (header.compressed_page_size_ < 0 ||
                 header.uncompressed_page_size_ < 0 ||
                 header.compressed_page_size_ > (1 << 30)) {
        ret = FS_ERR_BAD_DATA;
        parquet_set_err(err, "%s: invalid parquet page size", path_.c_str());
      } else {
        std::vector<uint8_t> raw(static_cast<size_t>(header.compressed_page_size_));
        const int64_t data_offset = offset + header_size;
        if (header.compressed_page_size_ > 0 &&
            static_cast<ssize_t>(raw.size()) !=
                pread(fd_, raw.data(), raw.size(), data_offset)) {
          ret = FS_ERR_IO;
          parquet_set_err(err, "read parquet page of '%s' failed", path_.c_str());
        } else {
          const uint8_t *page_data = raw.data();
          size_t page_len = raw.size();
          if (PC_SNAPPY == meta.codec_ && 0 == header.type_) {
            // data page v1: whole page compressed
            if (FS_OK != (ret = snappy_uncompress(raw.data(), raw.size(), page_buf, err))) {
            } else {
              page_data = page_buf.data();
              page_len = page_buf.size();
            }
          } else if (PC_SNAPPY == meta.codec_ && 3 == header.type_) {
            // data page v2: only values are compressed
            const size_t levels_len = static_cast<size_t>(header.v2_rep_len_ + header.v2_def_len_);
            if (levels_len > raw.size()) {
              ret = FS_ERR_BAD_DATA;
            } else if (header.v2_is_compressed_) {
              std::vector<uint8_t> values;
              if (FS_OK != (ret = snappy_uncompress(raw.data() + levels_len,
                                                    raw.size() - levels_len, values, err))) {
              } else {
                page_buf.assign(raw.begin(), raw.begin() + levels_len);
                page_buf.insert(page_buf.end(), values.begin(), values.end());
                page_data = page_buf.data();
                page_len = page_buf.size();
              }
            }
          } else if (PC_SNAPPY == meta.codec_ && 2 == header.type_) {
            // dictionary page is compressed as a whole
            if (FS_OK != (ret = snappy_uncompress(raw.data(), raw.size(), page_buf, err))) {
            } else {
              page_data = page_buf.data();
              page_len = page_buf.size();
            }
          }
          if (FS_OK == ret) {
            if (2 == header.type_) {
              // dictionary page
              dict_values.clear();
              const int64_t dict_cnt = header.dict_num_values_;
              ByteReader br(page_data, page_len);
              if (dict_cnt < 0 || dict_cnt > (1LL << 30)) {
                ret = FS_ERR_BAD_DATA;
              } else {
                for (int64_t i = 0; FS_OK == ret && i < dict_cnt; ++i) {
                  FileCell cell;
                  if (PT_BOOLEAN == leaf.physical_type_) {
                    uint32_t bits = 0;
                    if (FS_OK != (ret = br.read_u8(*reinterpret_cast<uint8_t *>(&bits)))) {
                    } else {
                      ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                          leaf.scale_, bits, 0, 0.0, std::string(),
                                          cell, err);
                    }
                  } else if (PT_INT32 == leaf.physical_type_) {
                    uint32_t v = 0;
                    if (FS_OK != (ret = br.read_le32(v))) {
                    } else {
                      ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                          leaf.scale_, 0, static_cast<int32_t>(v), 0.0,
                                          std::string(), cell, err);
                    }
                  } else if (PT_INT64 == leaf.physical_type_) {
                    uint64_t v = 0;
                    if (FS_OK != (ret = br.read_le64(v))) {
                    } else {
                      ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                          leaf.scale_, 0, static_cast<int64_t>(v), 0.0,
                                          std::string(), cell, err);
                    }
                  } else if (PT_FLOAT == leaf.physical_type_) {
                    uint32_t v = 0;
                    if (FS_OK != (ret = br.read_le32(v))) {
                    } else {
                      float f = 0.0f;
                      memcpy(&f, &v, sizeof(f));
                      ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                          leaf.scale_, 0, 0, static_cast<double>(f),
                                          std::string(), cell, err);
                    }
                  } else if (PT_DOUBLE == leaf.physical_type_) {
                    double d = 0.0;
                    if (FS_OK != (ret = br.read_double(d))) {
                    } else {
                      ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                          leaf.scale_, 0, 0, d, std::string(), cell, err);
                    }
                  } else if (PT_INT96 == leaf.physical_type_) {
                    std::string s;
                    if (FS_OK != (ret = br.skip(12))) {
                    } else {
                      cell.type_ = FT_DATETIME;
                      cell.datetime_us_ = 0;
                    }
                  } else {
                    uint32_t len = 0;
                    std::string s;
                    if (FS_OK != (ret = br.read_le32(len)) ||
                        FS_OK != (ret = br.read_bytes(s, len))) {
                    } else {
                      ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                          leaf.scale_, 0, 0, 0.0, s, cell, err);
                    }
                  }
                  if (FS_OK == ret) {
                    dict_values.push_back(cell);
                  }
                }
                has_dict = (FS_OK == ret);
              }
            } else if (0 == header.type_ || 3 == header.type_) {
              // data page
              const bool is_v2 = (3 == header.type_);
              const int64_t page_num_values = is_v2 ? header.v2_num_values_ : header.num_values_;
              const int32_t encoding = is_v2 ? header.v2_encoding_ : header.encoding_;
              if (page_num_values < 0 || page_num_values > (1LL << 40)) {
                ret = FS_ERR_BAD_DATA;
                parquet_set_err(err, "%s: invalid parquet data page value count", path_.c_str());
              } else {
                // definition levels
                std::vector<uint32_t> def_levels;
                ByteReader br(page_data, page_len);
                if (!optional) {
                  // no definition levels for required columns
                } else if (is_v2) {
                  if (header.v2_rep_len_ < 0 || header.v2_def_len_ < 0 ||
                      static_cast<size_t>(header.v2_rep_len_ + header.v2_def_len_) > page_len) {
                    ret = FS_ERR_BAD_DATA;
                  } else if (FS_OK != (ret = rle_decode(page_data + header.v2_rep_len_,
                                                        static_cast<size_t>(header.v2_def_len_),
                                                        1, page_num_values, def_levels))) {
                  }
                  if (FS_OK == ret) {
                    br.skip(static_cast<size_t>(header.v2_rep_len_ + header.v2_def_len_));
                  }
                } else {
                  uint32_t def_len = 0;
                  if (FS_OK != (ret = br.read_le32(def_len)) ||
                      def_len > br.remain()) {
                    ret = FS_ERR_BAD_DATA;
                  } else {
                    const uint8_t *def_data = br.cur();
                    if (FS_OK != (ret = rle_decode(def_data, def_len, 1, page_num_values,
                                                   def_levels))) {
                    } else if (FS_OK != (ret = br.skip(def_len))) {
                    }
                  }
                }
                if (FS_OK == ret) {
                  const int64_t value_cnt = optional
                      ? static_cast<int64_t>(std::count(def_levels.begin(), def_levels.end(), 1))
                      : page_num_values;
                  std::vector<uint32_t> indices;
                  std::vector<FileCell> decoded;
                  if (PE_PLAIN_DICTIONARY == encoding || PE_RLE_DICTIONARY == encoding) {
                    uint8_t bit_width = 0;
                    if (!has_dict) {
                      ret = FS_ERR_BAD_DATA;
                      parquet_set_err(err, "%s: dictionary encoded page without dictionary",
                              path_.c_str());
                    } else if (FS_OK != (ret = br.read_u8(bit_width))) {
                    } else if (FS_OK != (ret = rle_decode(br.cur(), br.remain(), bit_width,
                                                          value_cnt, indices))) {
                    } else {
                      for (int64_t i = 0; FS_OK == ret && i < value_cnt; ++i) {
                        if (indices[static_cast<size_t>(i)] >= dict_values.size()) {
                          ret = FS_ERR_BAD_DATA;
                          parquet_set_err(err, "%s: parquet dictionary index out of range", path_.c_str());
                        } else {
                          decoded.push_back(dict_values[indices[static_cast<size_t>(i)]]);
                        }
                      }
                    }
                  } else if (PE_PLAIN == encoding) {
                    for (int64_t i = 0; FS_OK == ret && i < value_cnt; ++i) {
                      FileCell cell;
                      if (PT_BOOLEAN == leaf.physical_type_) {
                        const size_t bit = static_cast<size_t>(i);
                        uint32_t v = 0;
                        if (br.remain() > (bit >> 3)) {
                          v = (br.cur()[bit >> 3] >> (bit & 7)) & 1;
                          ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                              leaf.scale_, v, 0, 0.0, std::string(), cell, err);
                          if (FS_OK == ret && 0 == ((i + 1) & 7)) {
                            ret = br.skip(1);
                          }
                        } else {
                          ret = FS_ERR_BAD_DATA;
                        }
                      } else if (PT_INT32 == leaf.physical_type_) {
                        uint32_t v = 0;
                        if (FS_OK != (ret = br.read_le32(v))) {
                        } else {
                          ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                              leaf.scale_, 0, static_cast<int32_t>(v), 0.0,
                                              std::string(), cell, err);
                        }
                      } else if (PT_INT64 == leaf.physical_type_) {
                        uint64_t v = 0;
                        if (FS_OK != (ret = br.read_le64(v))) {
                        } else {
                          ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                              leaf.scale_, 0, static_cast<int64_t>(v), 0.0,
                                              std::string(), cell, err);
                        }
                      } else if (PT_FLOAT == leaf.physical_type_) {
                        uint32_t v = 0;
                        if (FS_OK != (ret = br.read_le32(v))) {
                        } else {
                          float f = 0.0f;
                          memcpy(&f, &v, sizeof(f));
                          ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                              leaf.scale_, 0, 0, static_cast<double>(f),
                                              std::string(), cell, err);
                        }
                      } else if (PT_DOUBLE == leaf.physical_type_) {
                        double d = 0.0;
                        if (FS_OK != (ret = br.read_double(d))) {
                        } else {
                          ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                              leaf.scale_, 0, 0, d, std::string(), cell, err);
                        }
                      } else if (PT_INT96 == leaf.physical_type_) {
                        if (FS_OK != (ret = br.skip(12))) {
                        } else {
                          cell.type_ = FT_DATETIME;
                          cell.datetime_us_ = 0;
                        }
                      } else {
                        uint32_t len = 0;
                        std::string s;
                        if (FS_OK != (ret = br.read_le32(len)) ||
                            FS_OK != (ret = br.read_bytes(s, len))) {
                          ret = FS_ERR_BAD_DATA;
                        } else {
                          ret = convert_value(leaf.physical_type_, leaf.converted_type_,
                                              leaf.scale_, 0, 0, 0.0, s, cell, err);
                        }
                      }
                      if (FS_OK == ret) {
                        decoded.push_back(cell);
                      }
                    }
                  } else {
                    ret = FS_ERR_UNSUPPORTED;
                    parquet_set_err(err, "%s: parquet encoding %d is not supported",
                            path_.c_str(), encoding);
                  }
                  if (FS_OK == ret) {
                    // merge with definition levels
                    int64_t value_pos = 0;
                    const int64_t limit = std::min(num_rows - produced, page_num_values);
                    for (int64_t i = 0; FS_OK == ret && i < limit; ++i) {
                      const bool is_null = optional &&
                          (static_cast<size_t>(i) >= def_levels.size() ||
                           0 == def_levels[static_cast<size_t>(i)]);
                      if (is_null) {
                        cells[static_cast<size_t>(produced + i)].reset();
                      } else if (value_pos < static_cast<int64_t>(decoded.size())) {
                        cells[static_cast<size_t>(produced + i)] =
                            decoded[static_cast<size_t>(value_pos++)];
                      } else {
                        ret = FS_ERR_BAD_DATA;
                        parquet_set_err(err, "%s: parquet page has fewer values than definition levels",
                                path_.c_str());
                      }
                    }
                    produced += limit;
                  }
                }
              }
            } else {
              // index page or unknown page: skip
            }
            if (FS_OK == ret) {
              offset = data_offset + header.compressed_page_size_;
            }
          }
        }
      }
    }
    if (FS_OK == ret && produced < num_rows) {
      // missing values are treated as NULL (e.g. trailing columns)
      for (int64_t i = produced; i < num_rows; ++i) {
        cells[static_cast<size_t>(i)].reset();
      }
    }
  }
  return ret;
}

int ParquetFileReader::load_row_group(int64_t rg_idx, std::string &err)
{
  int ret = FS_OK;
  rg_cells_.assign(schema_.size(), std::vector<FileCell>());
  if (rg_idx < 0 || rg_idx >= static_cast<int64_t>(meta_.row_groups_.size())) {
    ret = FS_ERR_BAD_DATA;
  } else {
    const RowGroup &rg = meta_.row_groups_[static_cast<size_t>(rg_idx)];
    for (int64_t leaf_idx = 0; FS_OK == ret && leaf_idx < static_cast<int64_t>(leaves_.size()); ++leaf_idx) {
      const LeafColumn &leaf = leaves_[static_cast<size_t>(leaf_idx)];
      int64_t col_idx = leaf.file_column_;
      if (leaf.nested_ || col_idx < 0 ||
          col_idx >= static_cast<int64_t>(schema_.size()) ||
          (needed_.size() == schema_.size() && !needed_[static_cast<size_t>(col_idx)])) {
        // pruned column
        continue;
      }
      if (leaf_idx >= static_cast<int64_t>(rg.columns_.size()) ||
          !rg.columns_[static_cast<size_t>(leaf_idx)].has_meta_) {
        ret = FS_ERR_BAD_DATA;
        parquet_set_err(err, "%s: missing parquet column chunk", path_.c_str());
      } else {
        ret = decode_column(rg.columns_[static_cast<size_t>(leaf_idx)].meta_, leaf_idx,
                            rg.num_rows_, rg_cells_[static_cast<size_t>(col_idx)], err);
      }
    }
    if (FS_OK == ret) {
      for (size_t i = 0; i < rg_cells_.size(); ++i) {
        if (rg_cells_[i].empty()) {
          rg_cells_[i].assign(static_cast<size_t>(rg.num_rows_), FileCell());
        }
      }
    }
  }
  return ret;
}

int ParquetFileReader::next(FileRow &row, bool &eof, std::string &err)
{
  int ret = FS_OK;
  row.clear();
  eof = false;
  bool got = false;
  while (FS_OK == ret && !got) {
    if (current_rg_ >= static_cast<int64_t>(meta_.row_groups_.size())) {
      eof = true;
      got = true;
    } else if (row_in_rg_ >= meta_.row_groups_[static_cast<size_t>(current_rg_)].num_rows_) {
      ++current_rg_;
      row_in_rg_ = 0;
      rg_cells_.clear();
    } else if (rg_cells_.empty()) {
      ret = load_row_group(current_rg_, err);
    } else {
      row.resize(schema_.size());
      for (size_t i = 0; FS_OK == ret && i < schema_.size(); ++i) {
        if (i < rg_cells_.size() &&
            row_in_rg_ < static_cast<int64_t>(rg_cells_[i].size())) {
          row[i] = rg_cells_[i][static_cast<size_t>(row_in_rg_)];
        } else {
          row[i].reset();
        }
      }
      ++row_in_rg_;
      got = true;
    }
  }
  return ret;
}

} // anonymous namespace

int create_parquet_reader(const FileReadOptions &options,
                          const std::string &path,
                          FileScanReader *&reader,
                          std::string &err)
{
  int ret = FS_OK;
  reader = new (std::nothrow) ParquetFileReader(options, path);
  if (NULL == reader) {
    ret = FS_ERR_IO;
    parquet_set_err(err, "allocate parquet reader failed");
  }
  return ret;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
