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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ik/ob_ik_processor.h"

#include "lib/allocator/ob_allocator.h"
#include "lib/charset/ob_charset.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/ik/ob_ik_arbitrator.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
int ObIIKProcessor::process(TokenizeContext &ctx)
{
  int ret = OB_SUCCESS;

  ObFTCharUtil::CharType type;
  const char *ch = nullptr;
  uint8_t char_len = 0;

  if (OB_FAIL(ctx.current_char_type(type))) {
  } else if (OB_FAIL(ctx.current_char(ch, char_len))) {
  } else if (OB_FAIL(do_process(ctx, ch, char_len, type))) {
  }
  return ret;
}

int ObIIKProcessor::process(TokenizeContext &ctx,
                            const char *ch,
                            const uint8_t char_len,
                            const ObFTCharUtil::CharType type)
{
  return do_process(ctx, ch, char_len, type);
}

TokenizeContext::TokenizeContext(ObCollationType coll_type,
                                 ObIAllocator &allocator,
                                 const char *fulltext,
                                 int64_t fulltext_len,
                                 bool is_smart)
    : coll_type_(coll_type), cs_type_(ObCharset::charset_type_by_coll(coll_type)),
      fulltext_(fulltext), fulltext_len_(fulltext_len), cursor_(0),
      next_char_len_(0), handle_size_(0), is_smart_(is_smart), token_list_(allocator),
      result_list_(allocator)
{
}

int TokenizeContext::init()
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(fulltext_) || fulltext_len_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(prepare_next_char())) {
  }
  return ret;
}

int TokenizeContext::reset_resource()
{
  handle_size_ = 0;
  result_list_.reset();
  token_list_.reset();
  return OB_SUCCESS;
}

int TokenizeContext::prepare_next_char()
{
  int ret = OB_SUCCESS;
  const char *cur = fulltext_ + cursor_;
  const int64_t remain = fulltext_len_ - cursor_;
  bool handled = false;
  // Fast path: for well formed UTF-8 the length and the character class follow
  // directly from the leading byte, so the generic charset dispatch (a function
  // pointer call plus argument validation per character) can be skipped. Every
  // sequence this shortcut does not recognise falls back to the charset
  // routines, so the classification stays byte for byte identical.
  if (OB_LIKELY(common::CHARSET_UTF8MB4 == cs_type_ && remain > 0)) {
    const unsigned char b0 = static_cast<unsigned char>(cur[0]);
    if (b0 < 0x80) {
      next_char_len_ = 1;
      if ((b0 >= 0x41 && b0 <= 0x5a) || (b0 >= 0x61 && b0 <= 0x7a)) {
        next_char_type_ = ObFTCharUtil::CharType::ENGLISH_LETTER;
      } else if (b0 >= 0x30 && b0 <= 0x39) {
        next_char_type_ = ObFTCharUtil::CharType::ARABIC_LETTER;
      } else {
        next_char_type_ = ObFTCharUtil::CharType::USELESS;
      }
      handled = true;
    } else if (b0 >= 0xE4 && b0 <= 0xE9 && remain >= 3) {
      // U+4000..U+9FFF, the block that holds the CJK unified ideographs.
      const unsigned char b1 = static_cast<unsigned char>(cur[1]);
      const unsigned char b2 = static_cast<unsigned char>(cur[2]);
      if (OB_LIKELY(0x80 == (b1 & 0xC0) && 0x80 == (b2 & 0xC0))) {
        const ob_wc_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        next_char_len_ = 3;
        next_char_type_ = ObUnicodeBlockUtils::is_chinese(cp)
                              ? ObFTCharUtil::CharType::CHINESE
                              : ObFTCharUtil::CharType::USELESS;
        handled = true;
      }
    } else if (b0 >= 0xC2 && b0 <= 0xDF && remain >= 2) {
      // U+0080..U+07FF: no letter, digit or CJK block can appear here.
      if (OB_LIKELY(0x80 == (static_cast<unsigned char>(cur[1]) & 0xC0))) {
        next_char_len_ = 2;
        next_char_type_ = ObFTCharUtil::CharType::USELESS;
        handled = true;
      }
    }
  }
  if (OB_LIKELY(handled)) {
  } else if (OB_FAIL(ObCharset::first_valid_char(coll_type_,
                                                cur,
                                                remain,
                                                next_char_len_))) {
  } else if (OB_FAIL(ObFTCharUtil::classify_char(cs_type_,
                                                cur,
                                                next_char_len_,
                                                next_char_type_))) {
  }
  return ret;
}

int TokenizeContext::step_next()
{
  int ret = OB_SUCCESS;
  if (cursor_ >= fulltext_len_) {
    ret = OB_ITER_END;
  } else if (cursor_ + next_char_len_ >= fulltext_len_) {
    cursor_ = fulltext_len_;
    next_char_len_ = 0;
    ret = OB_ITER_END;
  } else if (cursor_ < fulltext_len_ && 0 == next_char_len_) {
    // should not happen
    ret = OB_UNEXPECT_INTERNAL_ERROR;
  } else {
    cursor_ += next_char_len_;
    handle_size_++;
    if (OB_FAIL(prepare_next_char())) {
    } else {
    }
  }
  return ret;
}

bool TokenizeContext::is_smart() const { return is_smart_; }

int TokenizeContext::add_token(const char *fulltext,
                               int64_t offset,
                               int64_t length,
                               int64_t char_cnt,
                               ObIKTokenType type)
{
  int ret = OB_SUCCESS;
  ObIKToken token;
  token.ptr_ = fulltext;
  token.length_ = length;
  token.offset_ = offset;
  token.char_cnt_ = char_cnt;
  token.type_ = type;
  if (OB_FAIL(token_list_.add_token(token))) {
  }
  return ret;
}

TokenizeContext::~TokenizeContext()
{
  token_list_.reset();
  result_list_.reset();
}

int TokenizeContext::get_next_token(const char *&word,
                                    int64_t &word_len,
                                    int64_t &offset,
                                    int64_t &char_cnt)
{
  int ret = OB_SUCCESS;
  if (!result_list_.empty()) {
    ObIKToken &token = result_list_.get_first();
    result_list_.pop_front();
    if (!result_list_.empty()) {
      if (OB_FAIL(compound(token))) {
      } else {
        // pass
      }
    }
    if (OB_SUCC(ret)) {
      word = token.ptr_;
      word_len = token.length_;
      offset = token.offset_;
      char_cnt = token.char_cnt_;
    }
  } else {
    ret = OB_ITER_END;
  }
  return ret;
}

int TokenizeContext::compound(ObIKToken &token)
{
  int ret = OB_SUCCESS;
  ObList<ObIKToken, ObIAllocator> &list = result_list_;
  if (is_smart_) {
    if (!list.empty()) {
      if (ObIKTokenType::IK_ARABIC_TOKEN == token.type_) {
        ObIKToken &next = list.get_first();
        bool append = false;

        if (ObIKTokenType::IK_CNNUM_TOKEN == next.type_) {
          // handle eng num + chn num
          if (token.offset_ + token.length_ == next.offset_) {
            append = true;
            token.length_ += next.length_;
            token.char_cnt_ += next.char_cnt_;
            token.type_ = ObIKTokenType::IK_CNNUM_TOKEN;
          }
        } else if (ObIKTokenType::IK_COUNT_TOKEN == next.type_) {
          // handle eng num + chn count
          if (token.offset_ + token.length_ == next.offset_) {
            append = true;
            token.length_ += next.length_;
            token.char_cnt_ += next.char_cnt_;
            token.type_ = ObIKTokenType::IK_CNQUAN_TOKEN;
          }
        } else {
          // pass
        }
        if (append) {
          list.pop_front();
        }
      }
      // There may be another round of append
      if (OB_SUCC(ret)) {
        ObIKToken next = list.get_first();
        bool append = false;
        if (ObIKTokenType::IK_COUNT_TOKEN == next.type_) {
          if (token.offset_ + token.length_ == next.offset_) {
            append = true;
            token.length_ += next.length_;
            token.type_ = ObIKTokenType::IK_CNQUAN_TOKEN;
          }
        }
        if (append) {
          list.pop_front();
        }
      }
    }
  } else {
    // nothing todo, just return
  }
  return ret;
}

} // namespace storage

} // namespace oceanbase
