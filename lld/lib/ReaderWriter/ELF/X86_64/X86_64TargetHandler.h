//===- lib/ReaderWriter/ELF/X86_64/X86_64TargetHandler.h ------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_X86_64_TARGET_HANDLER_H
#define LLD_READER_WRITER_ELF_X86_64_TARGET_HANDLER_H

#include "DefaultTargetHandler.h"

namespace lld {
namespace elf {
typedef llvm::object::ELFType<llvm::support::little, 8, true> X86_64ELFType;
class X86_64TargetInfo;

class X86_64TargetRelocationHandler LLVM_FINAL
    : public TargetRelocationHandler<X86_64ELFType> {
public:
  X86_64TargetRelocationHandler(const X86_64TargetInfo &ti) : _targetInfo(ti) {}

  virtual ErrorOr<void> applyRelocation(ELFWriter &, llvm::FileOutputBuffer &,
                                        const AtomLayout &,
                                        const Reference &)const;

private:
  const X86_64TargetInfo &_targetInfo;
};

class X86_64TargetHandler LLVM_FINAL
    : public DefaultTargetHandler<X86_64ELFType> {
public:
  X86_64TargetHandler(X86_64TargetInfo &targetInfo);

  virtual const X86_64TargetRelocationHandler &getRelocationHandler() const {
    return _relocationHandler;
  }

private:
  X86_64TargetRelocationHandler _relocationHandler;
};
} // end namespace elf
} // end namespace lld

#endif
