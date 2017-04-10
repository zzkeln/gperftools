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

#ifndef TCMALLOC_PAGE_HEAP_ALLOCATOR_H_
#define TCMALLOC_PAGE_HEAP_ALLOCATOR_H_

#include <stddef.h>                     // for NULL, size_t

#include "common.h"            // for MetaDataAlloc
#include "internal_logging.h"  // for ASSERT

namespace tcmalloc {

// Simple allocator for objects of a specified type.  External locking
// is required before accessing one of these objects.
template <class T>
class PageHeapAllocator {
 public:
  // We use an explicit Init function because these variables are statically
  // allocated and their constructors might not have run by the time some
  // other static variable tries to allocate memory.
  void Init() {
    ASSERT(sizeof(T) <= kAllocIncrement);
    inuse_ = 0;
    free_area_ = NULL;
    free_avail_ = 0;
    free_list_ = NULL;
    // Reserve some space at the beginning to avoid fragmentation.
   //这个调用会先分配128k内存，然后赋值给New一个T对象地址，Delete再释放这个T对象，故当前状态是free_list持有一个T对象地址
   //free_area有一个128k-sizeof(T)的起始地址，这个调用相当于默认分配一块内存
    Delete(New());
  }

  T* New() {
    // Consult free list
    void* result;
    
    if (free_list_ != NULL) {
     /*
     如果free_list不为空，那么拿出free_list作为首节点返回给用户，更新free_list为空闲链表的下一个节点即它指向节点的内容
     */
      result = free_list_;
      free_list_ = *(reinterpret_cast<void**>(result));
    } else {
      //如果此时空闲内存卡大小小于T对象的大小，那么要申请一块新的内存块来赋给free_area
      if (free_avail_ < sizeof(T)) {
        // Need more room. We assume that MetaDataAlloc returns
        // suitably aligned memory.
        //向系统申请一块内存
        free_area_ = reinterpret_cast<char*>(MetaDataAlloc(kAllocIncrement));
        if (free_area_ == NULL) {
          Log(kCrash, __FILE__, __LINE__,
              "FATAL ERROR: Out of memory trying to allocate internal "
              "tcmalloc data (bytes, object-size)",
              kAllocIncrement, sizeof(T));
        }
       //赋值新分配内存块的大小
        free_avail_ = kAllocIncrement;
      }
     //从空闲内存块移去T对象需要的大小
      result = free_area_;
      free_area_ += sizeof(T); //剩余的空闲内存块地址，向后移动T对象大小
      free_avail_ -= sizeof(T); //剩余空闲内存块大小还有多少
    }
    inuse_++; //又分配出去一个T对象
    return reinterpret_cast<T*>(result);
  }

 /*
  删除T对象，回收这块内存，使用这块内存来存放free_list的地址，然后更新free_list为这块内存地址，这样各个空闲的内存就串联起来了为一个单链表
  将p转换为void(**)然后取*并赋值为=free_list，即将p指向的对象的前面几个字节存储free_list的地址(类似于p->next=free_list)，然后更新free_list
 */
  void Delete(T* p) {
    *(reinterpret_cast<void**>(p)) = free_list_;
    free_list_ = p;
    inuse_--;
  }

  int inuse() const { return inuse_; }

 private:
  // How much to allocate from system at a time
  static const int kAllocIncrement = 128 << 10; //128k大小，x86的page是4k，故32个page

  // Free area from which to carve new objects
  char* free_area_; //空闲内存块起始地址
  size_t free_avail_;//空闲内存块大小

  // Free list of already carved objects
 /*
 注意这里仅仅用一个free_list就像空闲节点给串联起一个单链表，free_list指向表头。具体思想时，返回的内存节点T*p，其中p执行的T对象都是空闲的
 可以把它的内容当作next来使用，存放下一个节点的地址，这样就串联为一个单链表。这个void* free_list类似于struct T {T* next}；然后free_list是
 表头
 example:T是int，那么free_list就是一系列int*串联起来的单链表的头节点。当delete(int* p)时，将p指向的int内存存放free_list地址即当前链表首地址
 然后更新free_list=p，这样free_list指向单链表的头节点的地址，头节点的内容中存放下个节点的地址（返回内存是空闲的，所以头节点的内存中被利用起来了，
 当作next指针来使用）
 */
  void* free_list_; //已回收的空闲对象链表

  // Number of allocated but unfreed objects
  int inuse_;
};

}  // namespace tcmalloc

#endif  // TCMALLOC_PAGE_HEAP_ALLOCATOR_H_
