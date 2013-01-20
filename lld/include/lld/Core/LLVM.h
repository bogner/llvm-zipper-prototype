//===--- LLVM.h - Import various common LLVM datatypes ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file forward declares and imports various common LLVM datatypes that
// lld wants to use unqualified.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_LLVM_H
#define LLD_CORE_LLVM_H

// This should be the only #include, force #includes of all the others on
// clients.
#include "llvm/Support/Casting.h"

namespace llvm {
  // ADT's.
  class StringRef;
  class Twine;
  class MemoryBuffer;
  template<typename T> class ArrayRef;
  template<class T> class OwningPtr;
  template<unsigned InternalLen> class SmallString;
  template<typename T, unsigned N> class SmallVector;
  template<typename T> class SmallVectorImpl;

  template<typename T>
  struct SaveAndRestore;

  template<typename T>
  class ErrorOr;

  // Reference counting.
  template <typename T> class IntrusiveRefCntPtr;
  template <typename T> struct IntrusiveRefCntPtrInfo;
  template <class Derived> class RefCountedBase;
  class RefCountedBaseVPTR;

  class error_code;
  class raw_ostream;
  // TODO: DenseMap, ...
}

namespace lld {
  // Casting operators.
  using llvm::isa;
  using llvm::cast;
  using llvm::dyn_cast;
  using llvm::dyn_cast_or_null;
  using llvm::cast_or_null;

  // ADT's.
  using llvm::StringRef;
  using llvm::Twine;
  using llvm::MemoryBuffer;
  using llvm::ArrayRef;
  using llvm::OwningPtr;
  using llvm::SmallString;
  using llvm::SmallVector;
  using llvm::SmallVectorImpl;
  using llvm::SaveAndRestore;
  using llvm::ErrorOr;

  // Reference counting.
  using llvm::IntrusiveRefCntPtr;
  using llvm::IntrusiveRefCntPtrInfo;
  using llvm::RefCountedBase;
  using llvm::RefCountedBaseVPTR;

  using llvm::error_code;
  using llvm::raw_ostream;
} // end namespace clang.

#endif
