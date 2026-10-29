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

#include "storage/fts/dict/ob_gen_dic_loader.h"

#include "storage/fts/dict/ob_ik_utf8_dic_loader.h"
#include "storage/fts/ob_fts_literal.h"
#define USING_LOG_PREFIX STORAGE_FTS

namespace oceanbase
{
namespace storage
{
/**
 * -----------------------------------ObDicLoaderID-----------------------------------
 */
int ObGenDicLoader::ObGenDicLoaderKey::init(
    const ObString &parser_name, 
    const ObCharsetType charset)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(parser_name.empty() || CHARSET_INVALID == charset)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(set_parser_name(parser_name))) {
  } else {
    charset_ = charset;
  }
  return ret;
}
int ObGenDicLoader::ObGenDicLoaderKey::assign(const ObGenDicLoaderKey &other)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!other.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(set_parser_name(other.parser_name_))) {
  } else {
    charset_ = other.charset_;
  }
  return ret;
}
int ObGenDicLoader::ObGenDicLoaderKey::hash(uint64_t &hash_val) const
{
  hash_val = hash();
  return OB_SUCCESS;
}

uint64_t ObGenDicLoader::ObGenDicLoaderKey::hash() const
{
  uint64_t hash_val = 0;
  hash_val = murmurhash(&parser_name_, sizeof(parser_name_), 0);
  hash_val = murmurhash(&charset_, sizeof(charset_), hash_val);
  return hash_val;
}

int ObGenDicLoader::ObGenDicLoaderKey::set_parser_name(const char *parser_name)
{
  int ret = OB_SUCCESS;
  uint64_t len = STRLEN(parser_name);
  if (OB_ISNULL(parser_name)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(len >= OB_FT_PARSER_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMSET(parser_name_, '\0', OB_FT_PARSER_NAME_LENGTH);
    MEMCPY(parser_name_, parser_name, len);
  }
  return ret;
}

int ObGenDicLoader::ObGenDicLoaderKey::set_parser_name(const ObString &parser_name)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(parser_name.empty() || (parser_name.length() >= OB_FT_PARSER_NAME_LENGTH))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMSET(parser_name_, '\0', OB_FT_PARSER_NAME_LENGTH);
    MEMCPY(parser_name_, parser_name.ptr(), parser_name.length());
  }
  return ret;
}

/**
 * -----------------------------------ObDicLoader-----------------------------------
 */
int ObGenDicLoader::init()
{
  int ret = OB_SUCCESS;
  const uint64_t cap = 16;
  TCWLockGuard guard(lock_);
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (!dic_loader_map_.created() 
      && OB_FAIL(dic_loader_map_.create(cap, ObMemAttr("dic_loader_map")))) {
  } else {
    is_inited_ = true;
  }
  
  return ret;
}

namespace {
// The dictionary loader of one (parser, charset) pair never changes once it is
// created, and the global map keeps it alive for the whole process lifetime.
// Tokenizing a single document asks for the very same loader once per row, so
// cache the last hit on the thread to skip the key build, the hash lookup and
// the exclusive lock of the global map.
struct ObDicLoaderTLCache
{
  ObDicLoaderTLCache() : name_len_(0), charset_(CHARSET_INVALID), loader_(nullptr)
  {
    MEMSET(name_, '\0', OB_FT_PARSER_NAME_LENGTH);
  }
  char name_[OB_FT_PARSER_NAME_LENGTH];
  int32_t name_len_;
  ObCharsetType charset_;
  ObDicLoader *loader_;
};

thread_local ObDicLoaderTLCache TL_DIC_LOADER_CACHE;

OB_INLINE ObDicLoader *try_get_cached_dic_loader(const ObString &parser_name,
                                                 const ObCharsetType charset)
{
  ObDicLoader *loader = nullptr;
  const ObDicLoaderTLCache &cache = TL_DIC_LOADER_CACHE;
  if (nullptr != cache.loader_
      && cache.charset_ == charset
      && cache.name_len_ == parser_name.length()
      && 0 == MEMCMP(cache.name_, parser_name.ptr(), parser_name.length())) {
    loader = cache.loader_;
  }
  return loader;
}

OB_INLINE void save_cached_dic_loader(const ObString &parser_name,
                                      const ObCharsetType charset,
                                      ObDicLoader *loader)
{
  if (nullptr != loader && parser_name.length() < OB_FT_PARSER_NAME_LENGTH) {
    ObDicLoaderTLCache &cache = TL_DIC_LOADER_CACHE;
    MEMSET(cache.name_, '\0', OB_FT_PARSER_NAME_LENGTH);
    MEMCPY(cache.name_, parser_name.ptr(), parser_name.length());
    cache.name_len_ = static_cast<int32_t>(parser_name.length());
    cache.charset_ = charset;
    cache.loader_ = loader;
  }
}
} // namespace

int ObGenDicLoader::get_dic_loader(const ObString &parser_name, 
                                   const ObCharsetType charset, 
                                   ObDicLoaderHandle &loader_handle)
{
  int ret = OB_SUCCESS;
  ObGenDicLoaderKey dic_loader_key;
  ObDicLoader *dic_loader = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (parser_name.empty()
             || charset == CHARSET_INVALID) {
    ret = OB_INVALID_ARGUMENT;
  } else if (nullptr != (dic_loader = try_get_cached_dic_loader(parser_name, charset))) {
    if (OB_FAIL(loader_handle.set_loader(dic_loader))) {
    }
  } else if (OB_FAIL(dic_loader_key.init(parser_name, charset))) {
  } else {
    TCWLockGuard guard(lock_);
    if (OB_FAIL(dic_loader_map_.get_refactored(dic_loader_key, dic_loader))) {
      if (OB_HASH_NOT_EXIST == ret) {
        if (OB_FAIL(gen_dic_loader(dic_loader_key, dic_loader))) {
        } else if (OB_ISNULL(dic_loader)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(dic_loader_map_.set_refactored(dic_loader_key, dic_loader))) {
        } else if (OB_FALSE_IT(dic_loader->inc_ref())) {
        } else if (OB_FAIL(loader_handle.set_loader(dic_loader))) {
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
    } else if (OB_FAIL(loader_handle.set_loader(dic_loader))) {
    }
    if (OB_SUCC(ret)) {
      save_cached_dic_loader(parser_name, charset, dic_loader);
    }
  }
  return ret;
}

int ObGenDicLoader::gen_dic_loader(
    const ObGenDicLoaderKey &dic_loader_key, 
    ObDicLoader *&dic_loader)
{
  int ret = OB_SUCCESS;
  ObString parser_name = dic_loader_key.get_parser_name();
  ObCharsetType charset = dic_loader_key.get_charset();
  dic_loader = nullptr;
  if (nullptr != parser_name.find('.')) {
    parser_name = parser_name.split_on('.');
  }
  if (0 == parser_name.case_compare(ObFTSLiteral::PARSER_NAME_IK)) {
    ObMemAttr attr("dic_loader");
    switch (charset)
    {
      case ObCharsetType::CHARSET_UTF8MB4: {
        dic_loader = OB_NEW(ObIKUTF8DicLoader, attr);
        if (OB_ISNULL(dic_loader)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(dic_loader->init())) {
        }
        break;
      }
      default: {
        ret = OB_NOT_SUPPORTED;
        ObSqlString message;
        message.append_fmt("%s with the %s charset is",
                           ObFTSLiteral::PARSER_NAME_IK, ObCharset::charset_name(charset));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, message.ptr());
        break;
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "the parser is");
  }
  return ret;
}
} // end storage
} // end oceanbase
