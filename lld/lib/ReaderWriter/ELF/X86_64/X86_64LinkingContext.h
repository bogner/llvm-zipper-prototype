//===- lib/ReaderWriter/ELF/X86_64/X86_64LinkingContext.h -----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_X86_64_LINKER_CONTEXT_H
#define LLD_READER_WRITER_ELF_X86_64_LINKER_CONTEXT_H

#include "X86_64TargetHandler.h"

#include "lld/ReaderWriter/ELFLinkingContext.h"

#include "llvm/Object/ELF.h"
#include "llvm/Support/ELF.h"

namespace lld {
namespace elf {
/// \brief x86-64 internal references.
enum {
  /// \brief The 32 bit index of the relocation in the got this reference refers
  /// to.
  LLD_R_X86_64_GOTRELINDEX = 1024,
};

class X86_64LinkingContext LLVM_FINAL : public ELFLinkingContext {
public:
  X86_64LinkingContext(llvm::Triple triple)
      : ELFLinkingContext(triple, std::unique_ptr<TargetHandlerBase>(
                                      new X86_64TargetHandler(*this))) {}

  virtual void addPasses(PassManager &) const;

  virtual uint64_t getBaseAddress() const {
    if (_baseAddress == 0)
      return 0x400000;
    return _baseAddress;
  }

  virtual bool isDynamicRelocation(const DefinedAtom &,
                                   const Reference &r) const {
    switch (r.kind()) {
    case llvm::ELF::R_X86_64_RELATIVE:
    case llvm::ELF::R_X86_64_GLOB_DAT:
      return true;
    default:
      return false;
    }
  }

  virtual bool isPLTRelocation(const DefinedAtom &, const Reference &r) const {
    switch (r.kind()) {
    case llvm::ELF::R_X86_64_JUMP_SLOT:
    case llvm::ELF::R_X86_64_IRELATIVE:
      return true;
    default:
      return false;
    }
  }

  /// \brief X86_64 has two relative relocations
  /// a) for supporting IFUNC - R_X86_64_IRELATIVE
  /// b) for supporting relative relocs - R_X86_64_RELATIVE
  virtual bool isRelativeReloc(const Reference &r) const {
    switch (r.kind()) {
    case llvm::ELF::R_X86_64_IRELATIVE:
    case llvm::ELF::R_X86_64_RELATIVE:
      return true;
    default:
      return false;
    }
  }

  virtual ErrorOr<Reference::Kind> relocKindFromString(StringRef str) const;
  virtual ErrorOr<std::string> stringFromRelocKind(Reference::Kind kind) const;
};
} // end namespace elf
} // end namespace lld

#endif
