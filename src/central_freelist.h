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

#ifndef TCMALLOC_CENTRAL_FREELIST_H_
#define TCMALLOC_CENTRAL_FREELIST_H_

#include "config.h"
#include <stddef.h>                     // for size_t
#ifdef HAVE_STDINT_H
#include <stdint.h>                     // for int32_t
#endif
#include "base/spinlock.h"
#include "base/thread_annotations.h"
#include "common.h"
#include "span.h"

namespace tcmalloc {

 /*
 central cache是所有线程共享的缓冲区，因此对central cache的访问需要加锁。
 central cache所有的数据都在Static::central_cache_ [kNumClasses]中，即采用数组的方式来管理所有的cache，每个sizeclass
 的central cache对应一个数组元素，所以对不同sizeclass的central cache的访问是不冲突的，对cache的管理主要由类CentralFreeList来实现。
 CentralFreeListPadded Static::central_cache_[kNumClasses];
 */
 
// Data kept per size-class in central cache.
//每个sizeclass对应一个CentralFreeList，由init函数来设置sizeclass
class CentralFreeList {
 public:
  // A CentralFreeList may be used before its constructor runs.
  // So we prevent lock_'s constructor from doing anything to the
  // lock_ state.
  CentralFreeList() : lock_(base::LINKER_INITIALIZED) { }

  // 初始化,cl表示自己是第几个class
  void Init(size_t cl);

  // These methods all do internal locking.

  // Insert the specified range into the central freelist.  N is the number of
  // elements in the range.  RemoveRange() is the opposite operation.
  // 回收部分N个object,优先放入tc中
  void InsertRange(void *start, void *end, int N);

  // Returns the actual number of fetched elements and sets *start and *end.
  // 分配N个object, start和end是输出参数
  int RemoveRange(void **start, void **end, int N);

  // Returns the number of free objects in cache.
  // 在cache里面存在多少个free objects(不包含transfer cache),即nonempty span里有多少空闲的object数量
  int length() {
    SpinLockHolder h(&lock_);
    return counter_;
  }

  // Returns the number of free objects in the transfer cache.
  // transfer cache（tc_slots）里面包含多少free objects.
  int tc_length();

  // Returns the memory overhead (internal fragmentation) attributable
  // to the freelist.  This is memory lost when the size of elements
  // in a freelist doesn't exactly divide the page-size (an 8192-byte
  // page full of 5-byte objects would have 2 bytes memory overhead).
  // 因为内部碎片造成的额外开销，因为当前central_cache对应的sizeclass的字节可能是5字节，那么1page 8192字节不能整除5，会有2字节的内存碎片
  size_t OverheadBytes();

  // Lock/Unlock the internal SpinLock. Used on the pthread_atfork call
  // to set the lock in a consistent state before the fork.
  void Lock() {
    lock_.Lock();
  }

  void Unlock() {
    lock_.Unlock();
  }

 private:
  // TransferCache is used to cache transfers of
  // sizemap.num_objects_to_move(size_class) back and forth between
  // thread caches and the central cache for a given size class.
  // transfer_cache用来cache住thread_cache和central_freelist之间移动的object（所以tc_slots每个链表的object数量是num_objects_to_move）。
  //thread_cache将num_object_to_move个object返回给central_freelist时，会占用一个slot，且设置head和tail为返还内存的起始和结尾地址
 //一个tc链表的object数量肯定等于num_objects_to_move，这样不管是将这个slot分配给thread_cache还是将thread_cache还回来的num_object_to_move
 //放回slot，都只要设置链表的头尾节点即可，中间部分不用动，这样最高效
  struct TCEntry {
    void *head;  // Head of chain of objects.
    void *tail;  // Tail of chain of objects.
  };

  // A central cache freelist can have anywhere from 0 to kMaxNumTransferEntries
  // slots to put link list chains into.
#ifdef TCMALLOC_SMALL_BUT_SLOW
  // For the small memory model, the transfer cache is not used.
  static const int kMaxNumTransferEntries = 0;
#else
  // Starting point for the the maximum number of entries in the transfer cache.
  // This actual maximum for a given size class may be lower than this
  // maximum value.
  static const int kMaxNumTransferEntries = 64;
#endif

  // REQUIRES: lock_ is held
  // Remove object from cache and return.
  // Return NULL if no free entries in cache.
  //从non-empty链表的首节点分配一定数目（争取分配N个）的object出去，返回分配的数目
  int FetchFromOneSpans(int N, void **start, void **end) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // REQUIRES: lock_ is held
  // Remove object from cache and return.  Fetches
  // from pageheap if cache is empty.  Only returns
  // NULL on allocation failure.
 //争取分配N个object的内存，如果当前central_freelist中不存在任何object(即non-empty链表为空)，
 //那么从os分配一定的pages然后进行划分为多个object
 //加入non-empty span链表中
  int FetchFromOneSpansSafe(int N, void **start, void **end) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // REQUIRES: lock_ is held
  // Release a linked list of objects to spans.
  // May temporarily release lock_.
  void ReleaseListToSpans(void *start) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // REQUIRES: lock_ is held
  // Release an object to spans.
  // May temporarily release lock_.
  void ReleaseToSpans(void* object) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // REQUIRES: lock_ is held
  // Populate cache by fetching from the page heap.
  // May temporarily release lock_.
  //从系统申请一定的page过来，然后划分为多个object，作为一个span节点加入non-empty链表中
  void Populate() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // REQUIRES: lock is held.
  // Tries to make room for a TCEntry.  If the cache is full it will try to
  // expand it at the cost of some other cache size.  Return false if there is
  // no space.
 //争取弄一个空闲的tc_slot出来
  bool MakeCacheSpace() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // REQUIRES: lock_ for locked_size_class is held.
  // Picks a "random" size class to steal TCEntry slot from.  In reality it
  // just iterates over the sizeclasses but does so without taking a lock.
  // Returns true on success.
  // May temporarily lock a "random" size class.
  static bool EvictRandomSizeClass(int locked_size_class, bool force);

  // REQUIRES: lock_ is *not* held.
  // Tries to shrink the Cache.  If force is true it will relase objects to
  // spans if it allows it to shrink the cache.  Return false if it failed to
  // shrink the cache.  Decrements cache_size_ on succeess.
  // May temporarily take lock_.  If it takes lock_, the locked_size_class
  // lock is released to keep the thread from holding two size class locks
  // concurrently which could lead to a deadlock.
  bool ShrinkCache(int locked_size_class, bool force) LOCKS_EXCLUDED(lock_);

  // This lock protects all the data members.  cached_entries and cache_size_
  // may be looked at without holding the lock.
  SpinLock lock_;

  // We keep linked lists of empty and non-empty spans.
  size_t   size_class_;     // My size class 当前central_freelist对应的sizeclass
  Span     empty_;          // Dummy header for list of empty spans，empty span链表的表头
  Span     nonempty_;       // Dummy header for list of non-empty spans，non-empty span链表的表头
  size_t   num_spans_;      // Number of spans in empty_ plus nonempty_, span对象的个数
  size_t   counter_;        // Number of free objects in cache entry，

  // Here we reserve space for TCEntry cache slots.  Space is preallocated
  // for the largest possible number of entries than any one size class may
  // accumulate.  Not all size classes are allowed to accumulate
  // kMaxNumTransferEntries, so there is some wasted space for those size
  // classes.
  TCEntry tc_slots_[kMaxNumTransferEntries];

  // Number of currently used cached entries in tc_slots_.  This variable is
  // updated under a lock but can be read without one.
  int32_t used_slots_; //当前使用的tc_entries
  // The current number of slots for this size class.  This is an
  // adaptive value that is increased if there is lots of traffic
  // on a given size class.
  int32_t cache_size_; //当前允许的最大的tc entries.
  // Maximum size of the cache for a given size class.
  int32_t max_cache_size_;// 最大允许多少个tc entries.
};

// Pads each CentralCache object to multiple of 64 bytes.  Since some
// compilers (such as MSVC) don't like it when the padding is 0, I use
// template specialization to remove the padding entirely when
// sizeof(CentralFreeList) is a multiple of 64.
template<int kFreeListSizeMod64>
class CentralFreeListPaddedTo : public CentralFreeList {
 private:
  char pad_[64 - kFreeListSizeMod64];
};

template<>
class CentralFreeListPaddedTo<0> : public CentralFreeList {
};

//这个类是占用一定的内存，来使得补齐64字节的倍数
//CentralFreeListPadded Static::central_cache_[kNumClasses];
class CentralFreeListPadded : public CentralFreeListPaddedTo<
  sizeof(CentralFreeList) % 64> {
};

}  // namespace tcmalloc

#endif  // TCMALLOC_CENTRAL_FREELIST_H_
