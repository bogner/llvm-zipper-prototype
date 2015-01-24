//===- lib/ReaderWriter/ELF/Hexagon/HexagonLinkingContext.cpp -------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "HexagonLinkingContext.h"
#include "HexagonTargetHandler.h"

using namespace lld::elf;

std::unique_ptr<lld::ELFLinkingContext>
HexagonLinkingContext::create(llvm::Triple triple) {
  if (triple.getArch() == llvm::Triple::hexagon)
    return std::unique_ptr<lld::ELFLinkingContext>(
             new HexagonLinkingContext(triple));
  return nullptr;
}

HexagonLinkingContext::HexagonLinkingContext(llvm::Triple triple)
    : ELFLinkingContext(triple, std::unique_ptr<TargetHandlerBase>(
                                    new HexagonTargetHandler(*this))) {}
