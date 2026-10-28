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

#ifndef _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_
#define _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/ob_errno.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_processor.h"

namespace oceanbase
{
namespace storage
{
class ObFTDictHub;

/**
 * @class ObIKAliveHits
 * @brief The alive hits of the CJK segmenter, stored in a small inline array.
 *
 * ObList allocates one node per hit and per character from the allocator and, to
 * erase one hit, walks the whole list again comparing the elements, while the number
 * of alive hits never exceeds the longest word of the dictionary(a handful).
 * The traversal order is kept identical to the list one: removing the current
 * element makes the next one take its place, so the scan continues with the element
 * which directly followed the removed one.
 */
class ObIKAliveHits final
{
public:
  static constexpr int64_t LOCAL_CAPACITY = 16;

  explicit ObIKAliveHits(ObIAllocator &alloc)
      : alloc_(alloc), hits_(reinterpret_cast<ObDATrieHit *>(local_buf_)),
        capacity_(LOCAL_CAPACITY), count_(0)
  {
  }
  ~ObIKAliveHits() { reset(); }

  int64_t count() const { return count_; }
  bool empty() const { return 0 == count_; }
  ObDATrieHit &at(int64_t idx) { return hits_[idx]; }
  // Keep the allocated buffer, the next tokenize reuses it.
  void clear() { count_ = 0; }
  void reset()
  {
    if (hits_ != reinterpret_cast<ObDATrieHit *>(local_buf_)) {
      alloc_.free(hits_);
      hits_ = reinterpret_cast<ObDATrieHit *>(local_buf_);
    }
    capacity_ = LOCAL_CAPACITY;
    count_ = 0;
  }

  int push_back(const ObDATrieHit &hit)
  {
    int ret = OB_SUCCESS;
    if (count_ == capacity_ && OB_FAIL(grow())) {
    } else {
      MEMCPY(static_cast<void *>(hits_ + count_),
             static_cast<const void *>(&hit),
             sizeof(ObDATrieHit));
      ++count_;
    }
    return ret;
  }

  // Remove the idx element and keep the order of the remaining ones.
  int remove(int64_t idx)
  {
    int ret = OB_SUCCESS;
    if (idx < 0 || idx >= count_) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      for (int64_t i = idx; i + 1 < count_; ++i) {
        MEMCPY(static_cast<void *>(hits_ + i),
               static_cast<const void *>(hits_ + i + 1),
               sizeof(ObDATrieHit));
      }
      --count_;
    }
    return ret;
  }

private:
  int grow()
  {
    int ret = OB_SUCCESS;
    const int64_t new_capacity = capacity_ * 2;
    const int64_t byte_size = new_capacity * static_cast<int64_t>(sizeof(ObDATrieHit));
    void *buf = nullptr;
    if (OB_ISNULL(buf = alloc_.alloc(byte_size))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, static_cast<const void *>(hits_), count_ * sizeof(ObDATrieHit));
      if (hits_ != reinterpret_cast<ObDATrieHit *>(local_buf_)) {
        alloc_.free(hits_);
      }
      hits_ = reinterpret_cast<ObDATrieHit *>(buf);
      capacity_ = new_capacity;
    }
    return ret;
  }

private:
  ObIAllocator &alloc_;
  ObDATrieHit *hits_;
  int64_t capacity_;
  int64_t count_;
  alignas(ObDATrieHit) char local_buf_[LOCAL_CAPACITY * sizeof(ObDATrieHit)];

private:
  DISALLOW_COPY_AND_ASSIGN(ObIKAliveHits);
};

class ObIKCJKProcessor : public ObIIKProcessor
{
public:
  ObIKCJKProcessor(const ObIFTDict &dict_main, ObIAllocator &alloc)
      : hits_(alloc), dict_main_(dict_main), cjk_start_(-1), cjk_end_(-1)
  {
  }
  ~ObIKCJKProcessor() override { hits_.reset(); }

public:
  int do_process(TokenizeContext &ctx,
                 const char *ch,
                 const uint8_t char_len,
                 const ObFTCharUtil::CharType type) override;

private:
  ObIKAliveHits hits_;
  const ObIFTDict &dict_main_;
  int64_t cjk_start_;
  int64_t cjk_end_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObIKCJKProcessor);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_
