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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_RANGE_DICT_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_RANGE_DICT_H_

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_vector.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict_iterator.h"

namespace oceanbase
{
namespace storage
{
class ObFTRangeDict final : public ObIFTDict
{
public:
  ObFTRangeDict(ObIAllocator &alloc,
                ObFTCacheRangeContainer *range_container,
                const ObFTDictDesc &desc)
      : is_inited_(false), desc_(desc), range_alloc_(lib::ObMemAttr("Range Dict")),
        range_dicts_(&range_alloc_), cp_ranges_(nullptr), cp_range_count_(0),
        range_container_(range_container)
  {
  }
  ~ObFTRangeDict() override{};

public:
  struct ObFTRange final
  {
    ObFTSingleWord start_;
    ObFTSingleWord end_;
    ObIFTDict *dict_; // a cache dict
  };

public:
  int init() override;
  int match(const ObString &single_word, ObDATrieHit &hit) const override;
  int match(const ObString &words, bool &is_match) const override;
  int match_with_hit(const ObString &single_word,
                     const ObDATrieHit &last_hit,
                     ObDATrieHit &hit) const override;

public:
  int build_dict_from_cache(const ObFTCacheRangeContainer &range_container);

  static int try_load_cache(const ObFTDictDesc &desc,
                            const uint32_t range_count,
                            ObFTCacheRangeContainer &range_container);
  static int build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &range_container);

  static int build_cache_from_ik_dict(const ObFTDictDesc &desc, ObFTCacheRangeContainer &range_container);

private:
  // build cache
  static int build_ranges(const ObFTDictDesc &desc,
                          ObIFTDictIterator &iter,
                          ObFTCacheRangeContainer &range_container);

  // build one range's cache
  static int build_one_range(const ObFTDictDesc &desc,
                             const int32_t range_id,
                             ObIFTDictIterator &iter,
                             ObFTCacheRangeContainer &container,
                             bool &build_next_range);

  static int build_ranges_concurrently_thread_pool(const ObFTDictDesc &desc,
                                                   ObIFTDictIterator &iter,
                                                   ObFTCacheRangeContainer &range_container);

private:
  void destroy()
  {
    for (int64_t i = 0; i < range_dicts_.size(); i++) {
      range_dicts_[i].dict_->~ObIFTDict(); // destroy dict
    }
    cp_ranges_ = nullptr;
    cp_range_count_ = 0;
    range_dicts_.reset();
    range_alloc_.reset();
  }

  int find_first_char_range(const ObString &single_word, ObIFTDict *&dict) const;

private:
  // A range holds the first character of its first and of its last dictionary word, and
  // the ranges are generated while iterating a sorted dictionary, so the intervals
  // [start_char, end_char] are sorted and never overlap.  Looking a character up then
  // only needs the code points of the boundaries: for well formed utf-8 the byte order
  // used by ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, ...) is exactly the code point order.
  // The index is validated against the collation while it is built; when anything is not
  // as expected (ill formed boundary, unsorted or overlapping ranges, null dict) the
  // index stays empty and the original linear strcmp scan answers the lookup.
  struct CpRange
  {
    uint32_t start_cp_;
    uint32_t end_cp_;
    ObIFTDict *dict_;
  };

  // Answer a lookup with the given index; cp_range_count == 0 means "no index", the
  // linear strcmp scan answers then.  Kept as a parameter so build_cp_index() can
  // verify a candidate index against the scan before installing it.
  int lookup_first_char_range(const ObString &single_word,
                              const CpRange *cp_ranges,
                              const int64_t cp_range_count,
                              ObIFTDict *&dict) const;

  // Build the code point index, best effort: on any surprise it leaves cp_ranges_ empty.
  void build_cp_index();
  // Decode exactly one well formed utf-8 character filling [ptr, ptr + len).
  static bool decode_char_cp(const char *ptr, const int32_t len, uint32_t &cp);

private:
  static constexpr int DEFAULT_KEY_PER_RANGE = 50000; // by estimated
  bool is_inited_;
  ObFTDictDesc desc_;
  ObArenaAllocator range_alloc_;
  ObVector<ObFTRange, ObArenaAllocator> range_dicts_;
  CpRange *cp_ranges_; // read only lookup index over range_dicts_
  int64_t cp_range_count_;
  ObFTCacheRangeContainer *range_container_; // only used to read cache

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTRangeDict);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_RANGE_DICT_H_
