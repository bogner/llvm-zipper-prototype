//===- Hash.h - PDB hash functions ------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_RAW_HASH_H
#define LLVM_DEBUGINFO_PDB_RAW_HASH_H

#include "llvm/ADT/StringRef.h"

namespace llvm {
namespace pdb {
uint32_t hashStringV1(StringRef Str);
uint32_t hashStringV2(StringRef Str);
}
}

#endif
