//===- lib/ReaderWriter/ELF/AArch64/AArch64RelocationHandler.h ------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef AARCH64_RELOCATION_HANDLER_H
#define AARCH64_RELOCATION_HANDLER_H

#include "AArch64TargetHandler.h"

namespace lld {
namespace elf {
typedef llvm::object::ELFType<llvm::support::little, 2, true> AArch64ELFType;

template <class ELFT> class AArch64TargetLayout;

class AArch64TargetRelocationHandler final : public TargetRelocationHandler {
public:
  std::error_code applyRelocation(ELFWriter &, llvm::FileOutputBuffer &,
                                  const lld::AtomLayout &,
                                  const Reference &) const override;
};

} // end namespace elf
} // end namespace lld

#endif // AArch64_RELOCATION_HANDLER_H
