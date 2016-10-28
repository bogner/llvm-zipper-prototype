//===- Memory.cpp -----------------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Memory.h"

llvm::BumpPtrAllocator lld::elf::BAlloc;
llvm::StringSaver lld::elf::Saver{BAlloc};

void lld::elf::freeArena() { BAlloc.Reset(); }
