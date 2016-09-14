//===-- asan_errors.h -------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of AddressSanitizer, an address sanity checker.
//
// ASan-private header for error structures.
//===----------------------------------------------------------------------===//
#ifndef ASAN_ERRORS_H
#define ASAN_ERRORS_H

#include "asan_descriptions.h"
#include "asan_scariness_score.h"
#include "sanitizer_common/sanitizer_common.h"

namespace __asan {

struct ErrorBase {
  ErrorBase() = default;
  explicit ErrorBase(u32 tid_) : tid(tid_) {}
  ScarinessScoreBase scariness;
  u32 tid;
};

struct ErrorStackOverflow : ErrorBase {
  uptr addr, pc, bp, sp;
  // ErrorStackOverflow never owns the context.
  void *context;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorStackOverflow() = default;
  ErrorStackOverflow(u32 tid, const SignalContext &sig)
      : ErrorBase(tid),
        addr(sig.addr),
        pc(sig.pc),
        bp(sig.bp),
        sp(sig.sp),
        context(sig.context) {
    scariness.Clear();
    scariness.Scare(10, "stack-overflow");
  }
  void Print();
};

struct ErrorDeadlySignal : ErrorBase {
  uptr addr, pc, bp, sp;
  // ErrorDeadlySignal never owns the context.
  void *context;
  int signo;
  SignalContext::WriteFlag write_flag;
  bool is_memory_access;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorDeadlySignal() = default;
  ErrorDeadlySignal(u32 tid, const SignalContext &sig, int signo_)
      : ErrorBase(tid),
        addr(sig.addr),
        pc(sig.pc),
        bp(sig.bp),
        sp(sig.sp),
        context(sig.context),
        signo(signo_),
        write_flag(sig.write_flag),
        is_memory_access(sig.is_memory_access) {
    scariness.Clear();
    if (is_memory_access) {
      if (addr < GetPageSizeCached()) {
        scariness.Scare(10, "null-deref");
      } else if (addr == pc) {
        scariness.Scare(60, "wild-jump");
      } else if (write_flag == SignalContext::WRITE) {
        scariness.Scare(30, "wild-addr-write");
      } else if (write_flag == SignalContext::READ) {
        scariness.Scare(20, "wild-addr-read");
      } else {
        scariness.Scare(25, "wild-addr");
      }
    } else {
      scariness.Scare(10, "signal");
    }
  }
  void Print();
};

struct ErrorDoubleFree : ErrorBase {
  // ErrorDoubleFree doesn't own the stack trace.
  const BufferedStackTrace *second_free_stack;
  HeapAddressDescription addr_description;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorDoubleFree() = default;
  ErrorDoubleFree(u32 tid, BufferedStackTrace *stack, uptr addr)
      : ErrorBase(tid), second_free_stack(stack) {
    CHECK_GT(second_free_stack->size, 0);
    GetHeapAddressInformation(addr, 1, &addr_description);
    scariness.Clear();
    scariness.Scare(42, "double-free");
  }
  void Print();
};

struct ErrorNewDeleteSizeMismatch : ErrorBase {
  // ErrorNewDeleteSizeMismatch doesn't own the stack trace.
  const BufferedStackTrace *free_stack;
  HeapAddressDescription addr_description;
  uptr delete_size;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorNewDeleteSizeMismatch() = default;
  ErrorNewDeleteSizeMismatch(u32 tid, BufferedStackTrace *stack, uptr addr,
                             uptr delete_size_)
      : ErrorBase(tid), free_stack(stack), delete_size(delete_size_) {
    GetHeapAddressInformation(addr, 1, &addr_description);
    scariness.Clear();
    scariness.Scare(10, "new-delete-type-mismatch");
  }
  void Print();
};

struct ErrorFreeNotMalloced : ErrorBase {
  // ErrorFreeNotMalloced doesn't own the stack trace.
  const BufferedStackTrace *free_stack;
  AddressDescription addr_description;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorFreeNotMalloced() = default;
  ErrorFreeNotMalloced(u32 tid, BufferedStackTrace *stack, uptr addr)
      : ErrorBase(tid),
        free_stack(stack),
        addr_description(addr, /*shouldLockThreadRegistry=*/false) {
    scariness.Clear();
    scariness.Scare(40, "bad-free");
  }
  void Print();
};

struct ErrorAllocTypeMismatch : ErrorBase {
  // ErrorAllocTypeMismatch doesn't own the stack trace.
  const BufferedStackTrace *dealloc_stack;
  HeapAddressDescription addr_description;
  AllocType alloc_type, dealloc_type;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorAllocTypeMismatch() = default;
  ErrorAllocTypeMismatch(u32 tid, BufferedStackTrace *stack, uptr addr,
                         AllocType alloc_type_, AllocType dealloc_type_)
      : ErrorBase(tid),
        dealloc_stack(stack),
        alloc_type(alloc_type_),
        dealloc_type(dealloc_type_) {
    GetHeapAddressInformation(addr, 1, &addr_description);
    scariness.Clear();
    scariness.Scare(10, "alloc-dealloc-mismatch");
  };
  void Print();
};

struct ErrorMallocUsableSizeNotOwned : ErrorBase {
  // ErrorMallocUsableSizeNotOwned doesn't own the stack trace.
  const BufferedStackTrace *stack;
  AddressDescription addr_description;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorMallocUsableSizeNotOwned() = default;
  ErrorMallocUsableSizeNotOwned(u32 tid, BufferedStackTrace *stack_, uptr addr)
      : ErrorBase(tid),
        stack(stack_),
        addr_description(addr, /*shouldLockThreadRegistry=*/false) {
    scariness.Clear();
  }
  void Print();
};

struct ErrorSanitizerGetAllocatedSizeNotOwned : ErrorBase {
  // ErrorSanitizerGetAllocatedSizeNotOwned doesn't own the stack trace.
  const BufferedStackTrace *stack;
  AddressDescription addr_description;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorSanitizerGetAllocatedSizeNotOwned() = default;
  ErrorSanitizerGetAllocatedSizeNotOwned(u32 tid, BufferedStackTrace *stack_,
                                         uptr addr)
      : ErrorBase(tid),
        stack(stack_),
        addr_description(addr, /*shouldLockThreadRegistry=*/false) {
    scariness.Clear();
  }
  void Print();
};

struct ErrorStringFunctionMemoryRangesOverlap : ErrorBase {
  // ErrorStringFunctionMemoryRangesOverlap doesn't own the stack trace.
  const BufferedStackTrace *stack;
  uptr length1, length2;
  AddressDescription addr1_description;
  AddressDescription addr2_description;
  const char *function;
  // VS2013 doesn't implement unrestricted unions, so we need a trivial default
  // constructor
  ErrorStringFunctionMemoryRangesOverlap() = default;
  ErrorStringFunctionMemoryRangesOverlap(u32 tid, BufferedStackTrace *stack_,
                                         uptr addr1, uptr length1_, uptr addr2,
                                         uptr length2_, const char *function_)
      : ErrorBase(tid),
        stack(stack_),
        length1(length1_),
        length2(length2_),
        addr1_description(addr1, length1, /*shouldLockThreadRegistry=*/false),
        addr2_description(addr2, length2, /*shouldLockThreadRegistry=*/false),
        function(function_) {
    char bug_type[100];
    internal_snprintf(bug_type, sizeof(bug_type), "%s-param-overlap", function);
    scariness.Clear();
    scariness.Scare(10, bug_type);
  }
  void Print();
};

// clang-format off
#define ASAN_FOR_EACH_ERROR_KIND(macro)    \
  macro(StackOverflow)                     \
  macro(DeadlySignal)                      \
  macro(DoubleFree)                        \
  macro(NewDeleteSizeMismatch)             \
  macro(FreeNotMalloced)                   \
  macro(AllocTypeMismatch)                 \
  macro(MallocUsableSizeNotOwned)          \
  macro(SanitizerGetAllocatedSizeNotOwned) \
  macro(StringFunctionMemoryRangesOverlap)
// clang-format on

#define ASAN_DEFINE_ERROR_KIND(name) kErrorKind##name,
#define ASAN_ERROR_DESCRIPTION_MEMBER(name) Error##name name;
#define ASAN_ERROR_DESCRIPTION_CONSTRUCTOR(name) \
  ErrorDescription(Error##name const &e) : kind(kErrorKind##name), name(e) {}
#define ASAN_ERROR_DESCRIPTION_PRINT(name) \
  case kErrorKind##name:                   \
    return name.Print();

enum ErrorKind {
  kErrorKindInvalid = 0,
  ASAN_FOR_EACH_ERROR_KIND(ASAN_DEFINE_ERROR_KIND)
};

struct ErrorDescription {
  ErrorKind kind;
  // We're using a tagged union because it allows us to have a trivially
  // copiable type and use the same structures as the public interface.
  //
  // We can add a wrapper around it to make it "more c++-like", but that would
  // add a lot of code and the benefit wouldn't be that big.
  union {
    ASAN_FOR_EACH_ERROR_KIND(ASAN_ERROR_DESCRIPTION_MEMBER)
  };

  ErrorDescription() { internal_memset(this, 0, sizeof(*this)); }
  ASAN_FOR_EACH_ERROR_KIND(ASAN_ERROR_DESCRIPTION_CONSTRUCTOR)

  bool IsValid() { return kind != kErrorKindInvalid; }
  void Print() {
    switch (kind) {
      ASAN_FOR_EACH_ERROR_KIND(ASAN_ERROR_DESCRIPTION_PRINT)
      case kErrorKindInvalid:
        CHECK(0);
    }
    CHECK(0);
  }
};

#undef ASAN_FOR_EACH_ERROR_KIND
#undef ASAN_DEFINE_ERROR_KIND
#undef ASAN_ERROR_DESCRIPTION_MEMBER
#undef ASAN_ERROR_DESCRIPTION_CONSTRUCTOR
#undef ASAN_ERROR_DESCRIPTION_PRINT

}  // namespace __asan

#endif  // ASAN_ERRORS_H
