//===-- llvm/Support/Threading.cpp- Control multithreading mode --*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines helper functions for running LLVM in a multi-threaded
// environment.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Threading.h"
#include "llvm/Config/config.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/thread.h"

#include <cassert>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

using namespace llvm;

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only TRULY operating system
//===          independent code.
//===----------------------------------------------------------------------===//

bool llvm::llvm_is_multithreaded() {
#if LLVM_ENABLE_THREADS != 0
  return true;
#else
  return false;
#endif
}

#if LLVM_ENABLE_THREADS == 0 ||                                                \
    (!defined(LLVM_ON_WIN32) && !defined(HAVE_PTHREAD_H))
// Support for non-Win32, non-pthread implementation.
void llvm::llvm_execute_on_thread(void (*Fn)(void *), void *UserData,
                                  unsigned RequestedStackSize) {
  (void)RequestedStackSize;
  Fn(UserData);
}

unsigned llvm::heavyweight_hardware_concurrency() { return 1; }

uint64_t llvm::get_threadid_np() { return 0; }

void llvm::set_thread_name(const Twine &Name) {}

void llvm::get_thread_name(SmallVectorImpl<char> &Name) { Name.clear(); }

#else

unsigned llvm::heavyweight_hardware_concurrency() {
  int NumPhysical = sys::getHostNumPhysicalCores();
  if (NumPhysical == -1)
    return thread::hardware_concurrency();
  return NumPhysical;
}

// Include the platform-specific parts of this class.
#ifdef LLVM_ON_UNIX
#include "Unix/Threading.inc"
#endif
#ifdef LLVM_ON_WIN32
#include "Windows/Threading.inc"
#endif

#endif
