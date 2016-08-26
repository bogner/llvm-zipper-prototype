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

namespace __asan {

struct ErrorBase {
  ScarinessScore scariness;
};

struct ErrorStackOverflow : ErrorBase {
  u32 tid;
  uptr addr, pc, bp, sp;
  // ErrorStackOverflow never owns the context.
  void *context;
  ErrorStackOverflow(const SignalContext &sig, u32 tid_)
      : tid(tid_),
        addr(sig.addr),
        pc(sig.pc),
        bp(sig.bp),
        sp(sig.sp),
        context(sig.context) {
    scariness.Scare(10, "stack-overflow");
  }
  void Print();
};

enum ErrorKind {
  kErrorKindInvalid = 0,
  kErrorKindStackOverflow,
};

struct ErrorDescription {
  ErrorKind kind;
  // We're using a tagged union because it allows us to have a trivially
  // copiable type and use the same structures as the public interface.
  //
  // We can add a wrapper around it to make it "more c++-like", but that would
  // add a lot of code and the benefit wouldn't be that big.
  union {
    ErrorStackOverflow stack_overflow;
  };
  ErrorDescription() { internal_memset(this, 0, sizeof(*this)); }
  ErrorDescription(const ErrorStackOverflow &e)  // NOLINT
      : kind(kErrorKindStackOverflow),
        stack_overflow(e) {}

  bool IsValid() { return kind != kErrorKindInvalid; }
  void Print() {
    switch (kind) {
      case kErrorKindStackOverflow:
        stack_overflow.Print();
        return;
      case kErrorKindInvalid:
        CHECK(0);
    }
    CHECK(0);
  }
};

}  // namespace __asan

#endif  // ASAN_ERRORS_H
