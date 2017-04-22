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

#ifndef TCMALLOC_THREAD_CACHE_H_
#define TCMALLOC_THREAD_CACHE_H_

#include <config.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>                    // for pthread_t, pthread_key_t
#endif
#include <stddef.h>                     // for size_t, NULL
#ifdef HAVE_STDINT_H
#include <stdint.h>                     // for uint32_t, uint64_t
#endif
#include <sys/types.h>                  // for ssize_t
#include "base/commandlineflags.h"
#include "common.h"
#include "linked_list.h"
#include "maybe_threads.h"
#include "page_heap_allocator.h"
#include "sampler.h"
#include "static_vars.h"

#include "common.h"            // for SizeMap, kMaxSize, etc
#include "internal_logging.h"  // for ASSERT, etc
#include "linked_list.h"       // for SLL_Pop, SLL_PopRange, etc
#include "page_heap_allocator.h"  // for PageHeapAllocator
#include "sampler.h"           // for Sampler
#include "static_vars.h"       // for Static

DECLARE_int64(tcmalloc_sample_parameter);

namespace tcmalloc {

//-------------------------------------------------------------------
// Data kept per thread
//-------------------------------------------------------------------
/*
Thread Cache就是每一个线程里面管理小对象分配的cache.tcmalloc应该是假设局部线程里面通常分配的都是小对象，
这样可以减少锁竞争。 而如果是分配大对象的话，那么会直接从page heap里面进行分配。如果本地小对象不够的话，
那么会尝试从central cache里面要
*/
 
class ThreadCache {
 public:
#ifdef HAVE_TLS
  enum { have_tls = true };
#else
  enum { have_tls = false };
#endif

  // All ThreadCache objects are kept in a linked list (for stats collection)
  //所有ThreadCache都放在一个链表中
  ThreadCache* next_;
  ThreadCache* prev_;

  void Init(pthread_t tid);  // 初始化
  void Cleanup();

  // Accessors (mostly just for printing stats)
  // cl这个sizeclass对应的freelist里的object有多少个
  int freelist_length(size_t cl) const { return list_[cl].length(); }

  // Total byte size in cache
  size_t Size() const { return size_; }

  // Allocate an object of the given size and class. The size given
  // must be the same as the size of the class in the size map.
  void* Allocate(size_t size, size_t cl); // 从class里面分配size大小
  void Deallocate(void* ptr, size_t size_class); // 将ptr放回class对应slab里面

  void Scavenge(); // 回收内存到central cache.就是文档里面说的GC

  int GetSamplePeriod();

  // Record allocation of "k" bytes.  Return true iff allocation
  // should be sampled
  bool SampleAllocation(size_t k); // 是否认为这次分配的k字节需要进行采样.

  static void         InitModule();  // 初始化模块
  static void         InitTSD(); //初始化thread storage data.
  static ThreadCache* GetThreadHeap(); 
  static ThreadCache* GetCache(); // thread cache.
  static ThreadCache* GetCacheIfPresent();
  static ThreadCache* GetCacheWhichMustBePresent();
  static ThreadCache* CreateCacheIfNecessary(); // 如果tc不存在就创建
  static void         BecomeIdle();  // 标记这个thread已经idle，所以可以释放这个tc了
  static void         BecomeTemporarilyIdle();
  static size_t       MinSizeForSlowPath();
  static void         SetMinSizeForSlowPath(size_t size);
  static void         SetUseEmergencyMalloc();
  static void         ResetUseEmergencyMalloc();
  static bool         IsUseEmergencyMalloc();

  static bool IsFastPathAllowed() { return MinSizeForSlowPath() != 0; }

  // Return the number of thread heaps in use.
  static inline int HeapsInUse();

  // Adds to *total_bytes the total number of bytes used by all thread heaps.
  // Also, if class_count is not NULL, it must be an array of size kNumClasses,
  // and this function will increment each element of class_count by the number
  // of items in all thread-local freelists of the corresponding size class.
  // REQUIRES: Static::pageheap_lock is held.
  static void GetThreadStats(uint64_t* total_bytes, uint64_t* class_count);

  // Sets the total thread cache size to new_size, recomputing the
  // individual thread cache sizes as necessary.
  // REQUIRES: Static::pageheap lock is held.
  static void set_overall_thread_cache_size(size_t new_size);
  static size_t overall_thread_cache_size() {
    return overall_thread_cache_size_;
  }

 private:
  //freelist就是对应的slab.本质上数据结构就是一个单向链表,毕竟这个分配对于顺序没有任何要求
  class FreeList {
   private:
    void*    list_;       // Linked list of nodes 空闲链表的表头

#ifdef _LP64
    // On 64-bit hardware, manipulating 16-bit values may be slightly slow.
    uint32_t length_;      // Current length. 当前空闲链表的长度是多少（长度是指空闲的Object有多少个）
    uint32_t lowater_;     // Low water mark for list length. 长度最少时候达到多少
    uint32_t max_length_;  // Dynamic max list length based on usage. // 认为的最大长度多少
    // Tracks the number of times a deallocation has caused
    // length_ > max_length_.  After the kMaxOverages'th time, max_length_
    // shrinks and length_overages_ is reset to zero.
    uint32_t length_overages_; // 超过最大长度的次数
#else
    // If we aren't using 64-bit pointers then pack these into less space.
    uint16_t length_;
    uint16_t lowater_;
    uint16_t max_length_;
    uint16_t length_overages_;
#endif

   public:
    void Init() {
      list_ = NULL;
      length_ = 0;
      lowater_ = 0;
      max_length_ = 1;
      length_overages_ = 0;
    }

    // Return current length of list
    //返回当前空闲链表的元素数量
    size_t length() const {
      return length_;
    }

    // Return the maximum length of the list.
    size_t max_length() const {
      return max_length_;
    }

    // Set the maximum length of the list.  If 'new_max' > length(), the
    // client is responsible for removing objects from the list.
    void set_max_length(size_t new_max) {
      max_length_ = new_max;
    }

    // Return the number of times that length() has gone over max_length().
    size_t length_overages() const {
      return length_overages_;
    }

    void set_length_overages(size_t new_count) {
      length_overages_ = new_count;
    }

    // Is list empty?
    bool empty() const {
      return list_ == NULL;
    }

    // Low-water mark management
    int lowwatermark() const { return lowater_; }
    void clear_lowwatermark() { lowater_ = length_; }

   //将ptr指向的object放入空闲链表，长度++
    void Push(void* ptr) {
      SLL_Push(&list_, ptr);
      length_++;
    }

   //从空闲链表拿出一个object，长度--
    void* Pop() {
      ASSERT(list_ != NULL);
      length_--;
      if (length_ < lowater_) lowater_ = length_;
      return SLL_Pop(&list_);
    }

   //当前空闲链表的下一个元素
    void* Next() {
      return SLL_Next(&list_);
    }

   //将start, end之间的object（此时start, end之间的object已经串联起来了，用前4或8个字节来放下个节点的地址）
   //放入空闲链表，更新表头为start
    void PushRange(int N, void *start, void *end) {
      SLL_PushRange(&list_, start, end);
      length_ += N;
    }

   //从空闲链表中移除N个节点，N个节点的头置为start, 尾置为end
    void PopRange(int N, void **start, void **end) {
      SLL_PopRange(&list_, N, start, end);
      ASSERT(length_ >= N);
      length_ -= N;
      if (length_ < lowater_) lowater_ = length_;
    }
  };

  // Gets and returns an object from the central cache, and, if possible,
  // also adds some objects of that size class to this thread cache.
  void* FetchFromCentralCache(size_t cl, size_t byte_size);

  // Releases some number of items from src.  Adjusts the list's max_length
  // to eventually converge on num_objects_to_move(cl).
  void ListTooLong(FreeList* src, size_t cl);

  // Releases N items from this thread cache.
  void ReleaseToCentralCache(FreeList* src, size_t cl, int N);

  // Increase max_size_ by reducing unclaimed_cache_space_ or by
  // reducing the max_size_ of some other thread.  In both cases,
  // the delta is kStealAmount.
  void IncreaseCacheLimit();
  // Same as above but requires Static::pageheap_lock() is held.
  void IncreaseCacheLimitLocked();

  // If TLS is available, we also store a copy of the per-thread object
  // in a __thread variable since __thread variables are faster to read
  // than pthread_getspecific().  We still need pthread_setspecific()
  // because __thread variables provide no way to run cleanup code when
  // a thread is destroyed.
  // We also give a hint to the compiler to use the "initial exec" TLS
  // model.  This is faster than the default TLS model, at the cost that
  // you cannot dlopen this library.  (To see the difference, look at
  // the CPU use of __tls_get_addr with and without this attribute.)
  // Since we don't really use dlopen in google code -- and using dlopen
  // on a malloc replacement is asking for trouble in any case -- that's
  // a good tradeoff for us.
#ifdef HAVE_TLS
  struct ThreadLocalData {
    ThreadCache* heap;
    // min_size_for_slow_path is 0 if heap is NULL or kMaxSize + 1 otherwise.
    // The latter is the common case and allows allocation to be faster
    // than it would be otherwise: typically a single branch will
    // determine that the requested allocation is no more than kMaxSize
    // and we can then proceed, knowing that global and thread-local tcmalloc
    // state is initialized.
    size_t min_size_for_slow_path;

    bool use_emergency_malloc;
    size_t old_min_size_for_slow_path;
  };
  static __thread ThreadLocalData threadlocal_data_ ATTR_INITIAL_EXEC;
#endif

  // Thread-specific key.  Initialization here is somewhat tricky
  // because some Linux startup code invokes malloc() before it
  // is in a good enough state to handle pthread_keycreate().
  // Therefore, we use TSD keys only after tsd_inited is set to true.
  // Until then, we use a slow path to get the heap object.
  static bool tsd_inited_;
  static pthread_key_t heap_key_;

  // Linked list of heap objects.  Protected by Static::pageheap_lock.
 //静态全局变量，将每个ThreadCache通过链表连接起来（thread_heaps_是表头，每个节点通过prev和next来连接）
  static ThreadCache* thread_heaps_;
  static int thread_heap_count_;

  // A pointer to one of the objects in thread_heaps_.  Represents
  // the next ThreadCache from which a thread over its max_size_ should
  // steal memory limit.  Round-robin through all of the objects in
  // thread_heaps_.  Protected by Static::pageheap_lock.
 //指向一个ThreadCache，表示当一个线程的内存使用超过max_size_时会从这个ThreadCache“偷”一点内存上限
  static ThreadCache* next_memory_steal_;

  // Overall thread cache size.  Protected by Static::pageheap_lock.
  static size_t overall_thread_cache_size_;

  // Global per-thread cache size.  Writes are protected by
  // Static::pageheap_lock.  Reads are done without any locking, which should be
  // fine as long as size_t can be written atomically and we don't place
  // invariants between this variable and other pieces of state.
  static volatile size_t per_thread_cache_size_; // 每个tc的大小 (4 << 20,4MB)

  // Represents overall_thread_cache_size_ minus the sum of max_size_
  // across all ThreadCaches.  Protected by Static::pageheap_lock.
  static ssize_t unclaimed_cache_space_;

  // This class is laid out with the most frequently used fields
  // first so that hot elements are placed on the same cache line.

  size_t        size_;                  // Combined size of data
  size_t        max_size_;              // size_ > max_size_ --> Scavenge()

  // We sample allocations, biased by the size of the allocation
  Sampler       sampler_;               // A sampler

 //ThreadCache里包括kNumClasses种sizeclass，每个都通过一个FreeList来串联起来，这是空闲链表的核心存储地方
  FreeList      list_[kNumClasses];     // Array indexed by size-class

 //当前ThreadCache所属的线程id
  pthread_t     tid_;                   // Which thread owns it
  bool          in_setspecific_;        // In call to pthread_setspecific?

  // Allocate a new heap. REQUIRES: Static::pageheap_lock is held.
  static ThreadCache* NewHeap(pthread_t tid);

  // Use only as pthread thread-specific destructor function.
  static void DestroyThreadCache(void* ptr);

  static void DeleteCache(ThreadCache* heap);
  static void RecomputePerThreadCacheSize();

  // Ensure that this class is cacheline-aligned. This is critical for
  // performance, as false sharing would negate many of the benefits
  // of a per-thread cache.
} CACHELINE_ALIGNED;

// Allocator for thread heaps
// This is logically part of the ThreadCache class, but MSVC, at
// least, does not like using ThreadCache as a template argument
// before the class is fully defined.  So we put it outside the class.
extern PageHeapAllocator<ThreadCache> threadcache_allocator;

inline int ThreadCache::HeapsInUse() {
  return threadcache_allocator.inuse();
}

inline bool ThreadCache::SampleAllocation(size_t k) {
#ifndef NO_TCMALLOC_SAMPLES
  return UNLIKELY(FLAGS_tcmalloc_sample_parameter > 0) && sampler_.SampleAllocation(k);
#else
  return false;
#endif
}
//Allocate就是从对应的slab里面分配出一个object.
//注意在Init时候的话每个tc里面是没有任何内容的。
inline void* ThreadCache::Allocate(size_t size, size_t cl) {
  ASSERT(size <= kMaxSize);
 //三个数组的index是cl， 这个object得大小是size字节
  ASSERT(size == Static::sizemap()->ByteSizeForClass(cl));

  FreeList* list = &list_[cl];
  if (UNLIKELY(list->empty())) {
    return FetchFromCentralCache(cl, size);// 如果list里面为空的话，那么尝试从cc的cl里面分配size出来
  }
  size_ -= size;// 如果存在的话那么就直接-size并且弹出一个元素（这个元素占用size个字节，并且sizeclass=cl）
  return list->Pop();
}

//释放ptr，对应的sizeclass=cl
inline void ThreadCache::Deallocate(void* ptr, size_t cl) {
  FreeList* list = &list_[cl];
 //当前ThreadCache占用的内存+cl对应的字节数
  size_ += Static::sizemap()->ByteSizeForClass(cl);// 释放了这个内存所以空闲大小增大
  ssize_t size_headroom = max_size_ - size_ - 1;// 在size上面的话还有多少空闲.

  // This catches back-to-back frees of allocs in the same size
  // class. A more comprehensive (and expensive) test would be to walk
  // the entire freelist. But this might be enough to find some bugs.
  //更严谨的判断是遍历空闲链表，判断每个元素的地址都不应该等于ptr，否则ptr是已经释放回来的内存了再释放个啥嘞
  ASSERT(ptr != list->Next());

  list->Push(ptr); // 归还ptr到freelist中
  ssize_t list_headroom =
      static_cast<ssize_t>(list->max_length()) - list->length();// 在长度上还有多少空闲

  // There are two relatively uncommon things that require further work.
  // In the common case we're done, and in that case we need a single branch
  // because of the bitwise-or trick that follows.
  if (UNLIKELY((list_headroom | size_headroom) < 0)) {// 这个部分应该是有任意一个<0的话，那么就应该进入。优化手段吧.
    if (list_headroom < 0) {// 如果当前长度>max_length的话，那么需要重新设置max_length.
      ListTooLong(list, cl);
    }
   // 条件相当 if(size_headroom < 0)
        // 因为ListTooLog会尝试修改size_所以这里重新判断..:(tricky:(.
    if (size_ >= max_size_) Scavenge(); // 如果当前size>max_size的话，那么需要进行GC.
  }
}

//GetThreadHeap非常简单直接从线程局部变量里面取出即可 
inline ThreadCache* ThreadCache::GetThreadHeap() {
#ifdef HAVE_TLS
  return threadlocal_data_.heap;
#else
  return reinterpret_cast<ThreadCache *>(
      perftools_pthread_getspecific(heap_key_));
#endif
}

inline ThreadCache* ThreadCache::GetCacheWhichMustBePresent() {
#ifdef HAVE_TLS
  ASSERT(threadlocal_data_.heap);
  return threadlocal_data_.heap;
#else
  ASSERT(perftools_pthread_getspecific(heap_key_));
  return reinterpret_cast<ThreadCache *>(
      perftools_pthread_getspecific(heap_key_));
#endif
}

inline ThreadCache* ThreadCache::GetCache() {
  ThreadCache* ptr = NULL;
  if (!tsd_inited_) {
    InitModule(); // 初始化模块
  } else {
    ptr = GetThreadHeap(); //直接查看是否存在
  }
  if (ptr == NULL) ptr = CreateCacheIfNecessary(); //不存在的话就创建出一个
  return ptr;
}

// In deletion paths, we do not try to create a thread-cache.  This is
// because we may be in the thread destruction code and may have
// already cleaned up the cache for this thread.
inline ThreadCache* ThreadCache::GetCacheIfPresent() {
#ifndef HAVE_TLS
  if (!tsd_inited_) return NULL;
#endif
  return GetThreadHeap();
}

inline size_t ThreadCache::MinSizeForSlowPath() {
#ifdef HAVE_TLS
  return threadlocal_data_.min_size_for_slow_path;
#else
  return 0;
#endif
}

inline void ThreadCache::SetMinSizeForSlowPath(size_t size) {
#ifdef HAVE_TLS
  threadlocal_data_.min_size_for_slow_path = size;
#endif
}

inline void ThreadCache::SetUseEmergencyMalloc() {
#ifdef HAVE_TLS
  threadlocal_data_.old_min_size_for_slow_path = threadlocal_data_.min_size_for_slow_path;
  threadlocal_data_.min_size_for_slow_path = 0;
  threadlocal_data_.use_emergency_malloc = true;
#endif
}

inline void ThreadCache::ResetUseEmergencyMalloc() {
#ifdef HAVE_TLS
  threadlocal_data_.min_size_for_slow_path = threadlocal_data_.old_min_size_for_slow_path;
  threadlocal_data_.use_emergency_malloc = false;
#endif
}

inline bool ThreadCache::IsUseEmergencyMalloc() {
#if defined(HAVE_TLS) && defined(ENABLE_EMERGENCY_MALLOC)
  return UNLIKELY(threadlocal_data_.use_emergency_malloc);
#else
  return false;
#endif
}


}  // namespace tcmalloc

#endif  // TCMALLOC_THREAD_CACHE_H_
