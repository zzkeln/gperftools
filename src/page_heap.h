// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat <opensource@google.com>

#ifndef TCMALLOC_PAGE_HEAP_H_
#define TCMALLOC_PAGE_HEAP_H_

#include <config.h>
#include <stddef.h>                     // for size_t
#ifdef HAVE_STDINT_H
#include <stdint.h>                     // for uint64_t, int64_t, uint16_t
#endif
#include <gperftools/malloc_extension.h>
#include "base/basictypes.h"
#include "common.h"
#include "packed-cache-inl.h"
#include "pagemap.h"
#include "span.h"

// We need to dllexport PageHeap just for the unittest.  MSVC complains
// that we don't dllexport the PageHeap members, but we don't need to
// test those, so I just suppress this warning.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4251)
#endif

// This #ifdef should almost never be set.  Set NO_TCMALLOC_SAMPLES if
// you're porting to a system where you really can't get a stacktrace.
// Because we control the definition of GetStackTrace, all clients of
// GetStackTrace should #include us rather than stacktrace.h.
#ifdef NO_TCMALLOC_SAMPLES
  // We use #define so code compiles even if you #include stacktrace.h somehow.
# define GetStackTrace(stack, depth, skip)  (0)
#else
# include <gperftools/stacktrace.h>
#endif

namespace base {
struct MallocRange;
}

namespace tcmalloc {

// -------------------------------------------------------------------------
// Map from page-id to per-page data
// -------------------------------------------------------------------------

// We use PageMap2<> for 32-bit and PageMap3<> for 64-bit machines.
// We also use a simple one-level cache for hot PageID-to-sizeclass mappings,
// because sometimes the sizeclass is all the information we need.

// Selector class -- general selector uses 3-level map
template <int BITS> class MapSelector {
 public:
  typedef TCMalloc_PageMap3<BITS-kPageShift> Type;
  typedef PackedCache<BITS-kPageShift, uint64_t> CacheType;
};

// A two-level map for 32-bit machines
template <> class MapSelector<32> {
 public:
  typedef TCMalloc_PageMap2<32-kPageShift> Type;
  typedef PackedCache<32-kPageShift, uint16_t> CacheType;
};

// -------------------------------------------------------------------------
// Page-level allocator
//  * Eager coalescing
//
// Heap for page-level allocation.  We allow allocating and freeing a
// contiguous runs of pages (called a "span").
// -------------------------------------------------------------------------

class PERFTOOLS_DLL_DECL PageHeap {
 public:
  PageHeap();

  // Allocate a run of "n" pages.  Returns zero if out of memory.
  // Caller should not pass "n == 0" -- instead, n should have
  // been rounded up already.
  Span* New(Length n);// 分配n个pages并且返回Span对象

  // Delete the span "[p, p+n-1]".
  // REQUIRES: span was returned by earlier call to New() and
  //           has not yet been deleted.
  void Delete(Span* span); // 删除Span对象管理的内存

  // Mark an allocated span as being used for small objects of the
  // specified size-class.
  // REQUIRES: span was returned by an earlier call to New()
  //           and has not yet been deleted.
  void RegisterSizeClass(Span* span, size_t sc);// 注册这个span对象管理的slab大小多少(0表示不是用于分配小内存)

  // Split an allocated span into two spans: one of length "n" pages
  // followed by another span of length "span->length - n" pages.
  // Modifies "*span" to point to the first span of length "n" pages.
  // Returns a pointer to the second span.
  //
  // REQUIRES: "0 < n < span->length"
  // REQUIRES: span->location == IN_USE
  // REQUIRES: span->sizeclass == 0
  Span* Split(Span* span, Length n); // 将当前的span切分，一个管理n个页面的span,一个是剩余的。

  // Return the descriptor for the specified page.  Returns NULL if
  // this PageID was not allocated previously.
  inline Span* GetDescriptor(PageID p) const { //根据PageID得到管理这个Page的Span对象
    return reinterpret_cast<Span*>(pagemap_.get(p));
  }

  // If this page heap is managing a range with starting page # >= start,
  // store info about the range in *r and return true.  Else return false.
  bool GetNextRange(PageID start, base::MallocRange* r); //如果page heap管理了>=start的span,那么返回这个信息

  // Page heap statistics
  struct Stats {
    Stats() : system_bytes(0), free_bytes(0), unmapped_bytes(0), committed_bytes(0) {}
    uint64_t system_bytes;    // Total bytes allocated from system
    uint64_t free_bytes;      // Total bytes on normal freelists
    uint64_t unmapped_bytes;  // Total bytes on returned freelists
    uint64_t committed_bytes;  // Bytes committed, always <= system_bytes_.

  };
  inline Stats stats() const { return stats_; }

  struct SmallSpanStats {
    // For each free list of small spans, the length (in spans) of the
    // normal and returned free lists for that size.
    int64 normal_length[kMaxPages];
    int64 returned_length[kMaxPages];
  };
  void GetSmallSpanStats(SmallSpanStats* result);

  // Stats for free large spans (i.e., spans with more than kMaxPages pages).
  struct LargeSpanStats {
    int64 spans;           // Number of such spans
    int64 normal_pages;    // Combined page length of normal large spans
    int64 returned_pages;  // Combined page length of unmapped spans
  };
  void GetLargeSpanStats(LargeSpanStats* result);

  bool Check();
  // Like Check() but does some more comprehensive checking.
  bool CheckExpensive();
  bool CheckList(Span* list, Length min_pages, Length max_pages,
                 int freelist);  // ON_NORMAL_FREELIST or ON_RETURNED_FREELIST

  // Try to release at least num_pages for reuse by the OS.  Returns
  // the actual number of pages released, which may be less than
  // num_pages if there weren't enough pages to release. The result
  // may also be larger than num_pages since page_heap might decide to
  // release one large range instead of fragmenting it into two
  // smaller released and unreleased ranges.
  Length ReleaseAtLeastNPages(Length num_pages); // 尝试至少释放num_pages个页面

  // Return 0 if we have no information, or else the correct sizeclass for p.
  // Reads and writes to pagemap_cache_ do not require locking.
  // The entries are 64 bits on 64-bit hardware and 16 bits on
  // 32-bit hardware, and we don't mind raciness as long as each read of
  // an entry yields a valid entry, not a partially updated entry.
  size_t GetSizeClassIfCached(PageID p) const { // 在cache中返回这个page id对应的slab class
    return pagemap_cache_.GetOrDefault(p, 0);
  }
  void CacheSizeClass(PageID p, size_t cl) const { pagemap_cache_.Put(p, cl); }// 在cache中存放page id对应的slab class.

  bool GetAggressiveDecommit(void) {return aggressive_decommit_;}
  void SetAggressiveDecommit(bool aggressive_decommit) {
    aggressive_decommit_ = aggressive_decommit;
  }

 private:
  // Allocates a big block of memory for the pagemap once we reach more than
  // 128MB
  static const size_t kPageMapBigAllocationThreshold = 128 << 20;

  // Minimum number of pages to fetch from system at a time.  Must be
  // significantly bigger than kBlockSize to amortize system-call
  // overhead, and also to reduce external fragementation.  Also, we
  // should keep this value big because various incarnations of Linux
  // have small limits on the number of mmap() regions per
  // address-space.
  // REQUIRED: kMinSystemAlloc <= kMaxPages;
  static const int kMinSystemAlloc = kMaxPages;

  // Never delay scavenging for more than the following number of
  // deallocated pages.  With 4K pages, this comes to 4GB of
  // deallocation.
  static const int kMaxReleaseDelay = 1 << 20;

  // If there is nothing to release, wait for so many pages before
  // scavenging again.  With 4K pages, this comes to 1GB of memory.
  static const int kDefaultReleaseDelay = 1 << 18;

  // Pick the appropriate map and cache types based on pointer size
  typedef MapSelector<kAddressBits>::Type PageMap;
  typedef MapSelector<kAddressBits>::CacheType PageMapCache;
  PageMap pagemap_; //pageID-> span*，即将pageID和span*关联起来
  mutable PageMapCache pagemap_cache_; //pageID-> sizeclass，即将pageID和sizeclass关联起来

  // We segregate spans of a given size into two circular linked
  // lists: one for normal spans, and one for spans whose memory
  // has been returned to the system.
  struct SpanList {
    Span        normal;
    Span        returned;// 其实对于这个部分没有必要区分的，因为代码里面大部分都是挂在normal这个链上的。
  };

  // List of free spans of length >= kMaxPages
  SpanList large_; // 对于>=kMaxPages的页面单独维护一个free list.

  // Array mapping from span length to a doubly linked list of free spans
  //SpanList free_[kMaxPages]数组是按页大小递增的，即free_[1]是存放管理1页的Span，free_[2]是存放管理2页的Span，依此类推
  SpanList free_[kMaxPages]; // 针对每个页面大小做的free list. 基本上1 page到kMaxPages个页都放这个数组中管理。
  
  /*
  span的状态只有三种，一种是IN_USE表示正在被使用，一种表示ON_NORMAL_FREELIST表示放在了normal freelist上面。 
  另外一种是ON_RETURNED_FREELIST表示放在returned freelist上面。这里简单地说明一下normal freelist与returned freelist差别。 
  normal freelist是普通的回收进行缓存起来，
  而returned freelist表示已经完全unmmap回到系统内存部分了。
  不过因为实际并没有交回给系统内存， 所以这两个仅仅是概念上面的差别. 
  */
  
  
  // Statistics on system, free, and unmapped bytes
  Stats stats_;

  //申请一个n pages的span返回回去
  Span* SearchFreeAndLargeLists(Length n);

  bool GrowHeap(Length n);

  // REQUIRES: span->length >= n
  // REQUIRES: span->location != IN_USE
  // Remove span from its free list, and move any leftover part of
  // span into appropriate free lists.  Also update "span" to have
  // length exactly "n" and mark it as non-free so it can be returned
  // to the client.  After all that, decrease free_pages_ by n and
  // return span.
  Span* Carve(Span* span, Length n);

  //逻辑可以说非常简单，但是如果之前看过文档的话需要知道这里面pagemap为什么需要set. 非常简单，
  //如果span管理的是[p..q]的范围的话，那么在pagemap里面只需要记录(p,span),(q,span). 
  //这样如果有一个span回收的话，那么在pagemap里面查找p-1和q+1的span,然后尝试合并。
  //非常精巧。 所以在RecordSpan里面很明显就是需要设置前后的边界 
  void RecordSpan(Span* span) {
    pagemap_.set(span->start, span);
    if (span->length > 1) {
      pagemap_.set(span->start + span->length - 1, span);
    }
  }

  // Allocate a large span of length == n.  If successful, returns a
  // span of exactly the specified length.  Else, returns NULL.
  Span* AllocLarge(Length n);

  // Coalesce span with neighboring spans if possible, prepend to
  // appropriate free list, and adjust stats.
  void MergeIntoFreeList(Span* span);

  // Commit the span.
  void CommitSpan(Span* span);

  // Decommit the span.
  bool DecommitSpan(Span* span);

  // Prepends span to appropriate free list, and adjusts stats.
  void PrependToFreeList(Span* span);

  // Removes span from its free list, and adjust stats.
  void RemoveFromFreeList(Span* span);

  // Incrementally release some memory to the system.
  // IncrementalScavenge(n) is called whenever n pages are freed.
  void IncrementalScavenge(Length n);

  // Release the last span on the normal portion of this list.
  // Return the length of that span or zero if release failed.
  Length ReleaseLastNormalSpan(SpanList* slist);

  // Checks if we are allowed to take more memory from the system.
  // If limit is reached and allowRelease is true, tries to release
  // some unused spans.
  bool EnsureLimit(Length n, bool allowRelease = true);

  bool MayMergeSpans(Span *span, Span *other);

  // Number of pages to deallocate before doing more scavenging
  int64_t scavenge_counter_;

  // Index of last free list where we released memory to the OS.
  int release_index_;

  bool aggressive_decommit_;
};

}  // namespace tcmalloc

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif  // TCMALLOC_PAGE_HEAP_H_
