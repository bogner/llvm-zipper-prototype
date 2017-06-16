//===- AVR.cpp ------------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Error.h"
#include "InputFiles.h"
#include "Memory.h"
#include "Symbols.h"
#include "Target.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

namespace {
class AVR final : public TargetInfo {
public:
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S,
                     const uint8_t *Loc) const override;
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
};
} // namespace

RelExpr AVR::getRelExpr(uint32_t Type, const SymbolBody &S,
                        const uint8_t *Loc) const {
  switch (Type) {
  case R_AVR_CALL:
    return R_ABS;
  default:
    error(toString(S.File) + ": unknown relocation type: " + toString(Type));
    return R_HINT;
  }
}

void AVR::relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const {
  switch (Type) {
  case R_AVR_CALL: {
    uint16_t Hi = Val >> 17;
    uint16_t Lo = Val >> 1;
    write16le(Loc, read16le(Loc) | ((Hi >> 1) << 4) | (Hi & 1));
    write16le(Loc + 2, Lo);
    break;
  }
  default:
    error(getErrorLocation(Loc) + "unrecognized reloc " + toString(Type));
  }
}

TargetInfo *elf::createAVRTargetInfo() { return make<AVR>(); }
