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

#include "config.h"
#include <algorithm>
#include "central_freelist.h"
#include "internal_logging.h"  // for ASSERT, MESSAGE
#include "linked_list.h"       // for SLL_Next, SLL_Push, etc
#include "page_heap.h"         // for PageHeap
#include "static_vars.h"       // for Static

using std::min;
using std::max;

namespace tcmalloc {

//init主要是计算了tc(transfer cache)的max_cache_size以及cache_size,然后初始化了字段
void CentralFreeList::Init(size_t cl) {
  size_class_ = cl;
  tcmalloc::DLL_Init(&empty_);
  tcmalloc::DLL_Init(&nonempty_);
  num_spans_ = 0;
  counter_ = 0;

  max_cache_size_ = kMaxNumTransferEntries;
#ifdef TCMALLOC_SMALL_BUT_SLOW
  // Disable the transfer cache for the small footprint case.
  cache_size_ = 0;
#else
  cache_size_ = 16;
#endif
  if (cl > 0) {
    // Limit the maximum size of the cache based on the size class.  If this
    // is not done, large size class objects will consume a lot of memory if
    // they just sit in the transfer cache.
    int32_t bytes = Static::sizemap()->ByteSizeForClass(cl);
    int32_t objs_to_move = Static::sizemap()->num_objects_to_move(cl);

    ASSERT(objs_to_move > 0 && bytes > 0);
    // Limit each size class cache to at most 1MB of objects or one entry,
    // whichever is greater. Total transfer cache memory used across all
    // size classes then can't be greater than approximately
    // 1MB * kMaxNumTransferEntries.
    // min and max are in parens to avoid macro-expansion on windows.
    max_cache_size_ = (min)(max_cache_size_,
                          (max)(1, (1024 * 1024) / (bytes * objs_to_move)));
    cache_size_ = (min)(cache_size_, max_cache_size_);
  }
  used_slots_ = 0;
  ASSERT(cache_size_ <= max_cache_size_);
}
//大致逻辑就是遍历start直到end, 然后对于每一个object调用ReleaseToSpans单独进行处理。 
void CentralFreeList::ReleaseListToSpans(void* start) {
  while (start) {
    void *next = SLL_Next(start);
    ReleaseToSpans(start);
    start = next;
  }
}

// MapObjectToSpan should logically be part of ReleaseToSpans.  But
// this triggers an optimization bug in gcc 4.5.0.  Moving to a
// separate function, and making sure that function isn't inlined,
// seems to fix the problem.  It also should be fixed for gcc 4.5.1.
static
#if __GNUC__ == 4 && __GNUC_MINOR__ == 5 && __GNUC_PATCHLEVEL__ == 0
__attribute__ ((noinline))
#endif
Span* MapObjectToSpan(void* object) {
  const PageID p = reinterpret_cast<uintptr_t>(object) >> kPageShift;
  Span* span = Static::pageheap()->GetDescriptor(p);
  return span;
}

//将这个object返回给span，如果span的object都回收回来了，那么会释放这个span即从central_freelist--->page_heap
void CentralFreeList::ReleaseToSpans(void* object) {
  /*
  这里有一个最重要的问题就是MapObjectToSpan,object是如何映射到span的。这里我们首先可以大致说一下， 就是tcmalloc因为是按照page来分配的，
  所以如果知道地址的话，那么其实就知道于第几个页。而span可以管理多个页， 这样的话就可以知道这个页是哪个span来管理的了。
  */
  Span* span = MapObjectToSpan(object);// 将object映射到span
  ASSERT(span != NULL);
  ASSERT(span->refcount > 0);

  // If span is empty, move it to non-empty list
  if (span->objects == NULL) { // 如果span上面没有任何free objects的话.
    tcmalloc::DLL_Remove(span);// 那么将span从原来挂载链表删除(empty).
    tcmalloc::DLL_Prepend(&nonempty_, span);//将这个span挂载到这个cc的nonempty链表上.
    Event(span, 'N', 0);
  }

  // The following check is expensive, so it is disabled by default
  if (false) {
    // Check that object does not occur in list
    int got = 0;
    for (void* p = span->objects; p != NULL; p = *((void**) p)) {
      ASSERT(p != object);
      got++;
    }
    ASSERT(got + span->refcount ==
           (span->length<<kPageShift) /
           Static::sizemap()->ByteSizeForClass(span->sizeclass));
  }

  counter_++; // 当前free objects增加了
  span->refcount--;// 这个span的ref count减少了.
  // span refcount表示里面有多少个objects分配出去了.
  if (span->refcount == 0) {// 如果==0的话，那么说明这个span可以回收了.
    Event(span, '#', 0);
    counter_ -= ((span->length<<kPageShift) /
                 Static::sizemap()->ByteSizeForClass(span->sizeclass));
    tcmalloc::DLL_Remove(span);
    --num_spans_;

    // Release central list lock while operating on pageheap
    lock_.Unlock();
    {
      SpinLockHolder h(Static::pageheap_lock());
      Static::pageheap()->Delete(span);// 将span回收pageheap里面去，这个地方可能会进行内存合并
    }
    lock_.Lock();
  } else {
    *(reinterpret_cast<void**>(object)) = span->objects;// 否则就将这个object挂在span链上.
    span->objects = object;
  }
}

bool CentralFreeList::EvictRandomSizeClass(
    int locked_size_class, bool force) {
  static int race_counter = 0;
  int t = race_counter++;  // Updated without a lock, but who cares.
  if (t >= kNumClasses) {
    while (t >= kNumClasses) {
      t -= kNumClasses;
    }
    race_counter = t;
  }
  ASSERT(t >= 0);
  ASSERT(t < kNumClasses);
  if (t == locked_size_class) return false;
  return Static::central_cache()[t].ShrinkCache(locked_size_class, force);
}

bool CentralFreeList::MakeCacheSpace() {
  // Is there room in the cache?
  if (used_slots_ < cache_size_) return true;
  // Check if we can expand this cache?
  if (cache_size_ == max_cache_size_) return false;
  // Ok, we'll try to grab an entry from some other size class.
  if (EvictRandomSizeClass(size_class_, false) ||
      EvictRandomSizeClass(size_class_, true)) {
    // Succeeded in evicting, we're going to make our cache larger.
    // However, we may have dropped and re-acquired the lock in
    // EvictRandomSizeClass (via ShrinkCache and the LockInverter), so the
    // cache_size may have changed.  Therefore, check and verify that it is
    // still OK to increase the cache_size.
    if (cache_size_ < max_cache_size_) {
      cache_size_++;
      return true;
    }
  }
  return false;
}


namespace {
class LockInverter {
 private:
  SpinLock *held_, *temp_;
 public:
  inline explicit LockInverter(SpinLock* held, SpinLock *temp)
    : held_(held), temp_(temp) { held_->Unlock(); temp_->Lock(); }
  inline ~LockInverter() { temp_->Unlock(); held_->Lock();  }
};
}

// This function is marked as NO_THREAD_SAFETY_ANALYSIS because it uses
// LockInverter to release one lock and acquire another in scoped-lock
// style, which our current annotation/analysis does not support.
//尝试将一个tc里的slot的object返还给span，空闲出一个slot
bool CentralFreeList::ShrinkCache(int locked_size_class, bool force)
    NO_THREAD_SAFETY_ANALYSIS {
  // Start with a quick check without taking a lock.
  if (cache_size_ == 0) return false;
  // We don't evict from a full cache unless we are 'forcing'.
  if (force == false && used_slots_ == cache_size_) return false;

  // Grab lock, but first release the other lock held by this thread.  We use
  // the lock inverter to ensure that we never hold two size class locks
  // concurrently.  That can create a deadlock because there is no well
  // defined nesting order.
  LockInverter li(&Static::central_cache()[locked_size_class].lock_, &lock_);
  ASSERT(used_slots_ <= cache_size_);
  ASSERT(0 <= cache_size_);
  if (cache_size_ == 0) return false;
  if (used_slots_ == cache_size_) {
    if (force == false) return false;
    // ReleaseListToSpans releases the lock, so we have to make all the
    // updates to the central list before calling it.
    cache_size_--;
    used_slots_--;
    ReleaseListToSpans(tc_slots_[used_slots_].head);
    return true;
  }
  cache_size_--;
  return true;
}

//为了回收[start,end]并且长度为N objects的内存链。首先注意它加了自选锁确保了线程安全。 
//然后有一个逻辑就是判断是否可以进入tc,如果不允许进入tc的话那么挂到链上去
void CentralFreeList::InsertRange(void *start, void *end, int N) { //N表示从thread_cache将N个当前sizeclass对应的object返还给central_freelist
  SpinLockHolder h(&lock_);
  
  if (N == Static::sizemap()->num_objects_to_move(size_class_) &&
    MakeCacheSpace()) { //这里我们可以简单地认为，它就是在计算tc_slots里面是否有slot可以分配.
    int slot = used_slots_++;
    ASSERT(slot >=0);
    ASSERT(slot < max_cache_size_);
    TCEntry *entry = &tc_slots_[slot];// 如果分配成功的话，那么直接挂载.
    entry->head = start;
    entry->tail = end;
    return;
  }
  ReleaseListToSpans(start); // 如果不允许挂到tc的话，那么就需要单独处理.
}

//这个接口就是为了尝试分配N个objects对象，然后将首地址尾地址给start和end.
//同样内部逻辑会判断是否可以从tc 中直接取出，如果可以取出的话那么分配就非常快。注意函数开始也尝试加锁了。 
int CentralFreeList::RemoveRange(void **start, void **end, int N) {
  ASSERT(N > 0);
  lock_.Lock();
  if (N == Static::sizemap()->num_objects_to_move(size_class_) &&
      used_slots_ > 0) { //// 如果可以直接从tc里面分配.
    int slot = --used_slots_;
    ASSERT(slot >= 0);
    TCEntry *entry = &tc_slots_[slot];
    *start = entry->head;
    *end = entry->tail;
    lock_.Unlock();
    return N;
  }

  int result = 0;
  *start = NULL;
  *end = NULL;
  // TODO: Prefetch multiple TCEntries?
  result = FetchFromOneSpansSafe(N, start, end);// 逻辑是首先放在尾部,然后不断地在头部拼接.
  if (result != 0) {
    while (result < N) {
      int n;
      void* head = NULL;
      void* tail = NULL;
      n = FetchFromOneSpans(N - result, &head, &tail);
      if (!n) break;
      result += n;
      SLL_PushRange(start, head, tail);
    }
  }
  lock_.Unlock();
  return result;
}

//争取分配N个object的内存，如果当前central_freelist中不存在任何object(即non-empty链表为空)，那么从os分配一定的pages然后进行划分为多个object
//加入non-empty span链表中
int CentralFreeList::FetchFromOneSpansSafe(int N, void **start, void **end) {
  int result = FetchFromOneSpans(N, start, end);
  //如果result==0，说明non-empty链表为空了，那么
  if (!result) {
    Populate(); //向系统分配一定数量的pages，然后划分为多个object作为一个span节点加入non-empty span链表中
    result = FetchFromOneSpans(N, start, end); //从non-empty span链表中争取分配N个object，result为分配了多少个object
  }
  return result;
}

//从non-empty spans里分配N个object。返回值表示分配了多少个object出去
int CentralFreeList::FetchFromOneSpans(int N, void **start, void **end) {
  if (tcmalloc::DLL_IsEmpty(&nonempty_)) return 0; // 如果nonempty span里面都空了的直接返回0
  Span* span = nonempty_.next;//拿到non-empty链表的节点

  ASSERT(span->objects != NULL);

  int result = 0;
  void *prev, *curr;
  curr = span->objects;
  //遍历这个span的object链表
  do {
    prev = curr;
    curr = *(reinterpret_cast<void**>(curr));
  } while (++result < N && curr != NULL);

  //如果curr==NULL,那么这个span的object将要都被分配出去，这个span将变成空的了，那么移到empty链表中
  if (curr == NULL) {
    // Move to empty list
    tcmalloc::DLL_Remove(span); //从non-empty链表中移除
    tcmalloc::DLL_Prepend(&empty_, span); //加入empty链表中
    Event(span, 'E', 0);
  }

  *start = span->objects; //设置start为span的object的起始地址
  *end = prev; //设置结尾地址为第N个object的结束地址
  span->objects = curr;//更新span的objects
  SLL_SetNext(*end, NULL);
  span->refcount += result; //这个span已经被分配出去result个object
  counter_ -= result; //central_freelist剩余的object减去分配出去的object数量
  return result;//返回分配出去了多少个object
}

// Fetch memory from the system and add to the central cache freelist.
//从系统分配一定数量的pages，然后划分为多个object，作为一个span加入non-empty span链表中
void CentralFreeList::Populate() {
  // Release central list lock while operating on pageheap
  lock_.Unlock();
  //首先需要计算出我们需要多少个pages
  const size_t npages = Static::sizemap()->class_to_pages(size_class_);

  Span* span;
  {
    SpinLockHolder h(Static::pageheap_lock());
    span = Static::pageheap()->New(npages);// 分配到pages得到span.
    if (span) Static::pageheap()->RegisterSizeClass(span, size_class_);
  }
  if (span == NULL) {
    Log(kLog, __FILE__, __LINE__,
        "tcmalloc: allocation failed", npages << kPageShift);
    lock_.Lock();
    return;
  }
  ASSERT(span->length == npages);
  // Cache sizeclass info eagerly.  Locking is not necessary.
  // (Instead of being eager, we could just replace any stale info
  // about this span, but that seems to be no better in practice.)
  for (int i = 0; i < npages; i++) {// 将span和size_class之间关联起来
    Static::pageheap()->CacheSizeClass(span->start + i, size_class_);
  }

  // 对这个span里面的所有objects组织成链表形式
  // Split the block into pieces and add to the free-list
  // TODO: coloring of objects to avoid cache conflicts?
  void** tail = &span->objects;
  char* ptr = reinterpret_cast<char*>(span->start << kPageShift);
  char* limit = ptr + (npages << kPageShift);
  const size_t size = Static::sizemap()->ByteSizeForClass(size_class_);
  int num = 0;
  //将这块内存切分为多个object(每个大小为size)，然后用单链表串联起来（链表节点的内容为下个节点的地址）
  while (ptr + size <= limit) {
    *tail = ptr;
    tail = reinterpret_cast<void**>(ptr);
    ptr += size;
    num++;
  }
  ASSERT(ptr <= limit);
  *tail = NULL;
  span->refcount = 0; // No sub-object in use yet 还没从span里分配出任何object

  // Add span to list of non-empty spans
   // 将这个span加入nonempty链表的话需要加锁。
  lock_.Lock();
  tcmalloc::DLL_Prepend(&nonempty_, span);
  ++num_spans_;
  counter_ += num;
}

//返回被transfre-cache所保存的object数目，当前slot数目*每个slot存储的object个数（这个个数是thread_cache-->central_freelist的object个数）
int CentralFreeList::tc_length() {
  SpinLockHolder h(&lock_);
  return used_slots_ * Static::sizemap()->num_objects_to_move(size_class_);
}

size_t CentralFreeList::OverheadBytes() {
  SpinLockHolder h(&lock_);
  if (size_class_ == 0) {  // 0 holds the 0-sized allocations
    return 0;
  }
  const size_t pages_per_span = Static::sizemap()->class_to_pages(size_class_);
  const size_t object_size = Static::sizemap()->class_to_size(size_class_);
  ASSERT(object_size > 0);
  const size_t overhead_per_span = (pages_per_span * kPageSize) % object_size;
  return num_spans_ * overhead_per_span;
}

}  // namespace tcmalloc
