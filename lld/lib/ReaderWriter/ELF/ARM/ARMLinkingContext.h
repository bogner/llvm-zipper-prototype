//===--------- lib/ReaderWriter/ELF/ARM/ARMLinkingContext.h ---------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_ARM_ARM_LINKING_CONTEXT_H
#define LLD_READER_WRITER_ELF_ARM_ARM_LINKING_CONTEXT_H

#include "ARMTargetHandler.h"

#include "lld/ReaderWriter/ELFLinkingContext.h"

#include "llvm/Object/ELF.h"
#include "llvm/Support/ELF.h"

namespace lld {
namespace elf {

class ARMLinkingContext final : public ELFLinkingContext {
public:
  ARMLinkingContext(llvm::Triple triple)
      : ELFLinkingContext(triple, std::unique_ptr<TargetHandlerBase>(
                                      new ARMTargetHandler(*this))) {}

  void addPasses(PassManager &) override;

  uint64_t getBaseAddress() const override {
    if (_baseAddress == 0)
      return 0x400000;
    return _baseAddress;
  }
};
} // end namespace elf
} // end namespace lld

#endif
