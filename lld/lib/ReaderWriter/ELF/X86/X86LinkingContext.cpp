//===- lib/ReaderWriter/ELF/X86/X86LinkingContext.cpp ---------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "X86LinkingContext.h"
#include "X86TargetHandler.h"
#include "lld/Core/LLVM.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorOr.h"

using namespace lld;

elf::X86LinkingContext::X86LinkingContext(llvm::Triple triple)
    : ELFLinkingContext(triple, std::unique_ptr<TargetHandlerBase>(
                        new X86TargetHandler(*this))) {}
