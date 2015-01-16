//===- lld/ReaderWriter/ELF/Mips/MipsRelocationHandler.h ------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef LLD_READER_WRITER_ELF_MIPS_MIPS_RELOCATION_HANDLER_H
#define LLD_READER_WRITER_ELF_MIPS_MIPS_RELOCATION_HANDLER_H

#include "MipsLinkingContext.h"

namespace lld {
namespace elf {

class MipsTargetHandler;

class MipsTargetRelocationHandler final : public TargetRelocationHandler {
public:
  MipsTargetRelocationHandler(MipsTargetLayout<Mips32ElELFType> &layout,
                              ELFLinkingContext &targetInfo)
      : TargetRelocationHandler(targetInfo), _mipsTargetLayout(layout) {}

  std::error_code applyRelocation(ELFWriter &, llvm::FileOutputBuffer &,
                                  const lld::AtomLayout &,
                                  const Reference &) const override;

private:
  MipsTargetLayout<Mips32ElELFType> &_mipsTargetLayout;
};

} // elf
} // lld

#endif
