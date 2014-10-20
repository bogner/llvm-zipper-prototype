//===- lib/ReaderWriter/ELF/X86_64/X86_64TargetHandler.h ------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_X86_64_X86_64_TARGET_HANDLER_H
#define LLD_READER_WRITER_ELF_X86_64_X86_64_TARGET_HANDLER_H

#include "DefaultTargetHandler.h"
#include "TargetLayout.h"
#include "X86_64ELFFile.h"
#include "X86_64ELFReader.h"
#include "X86_64RelocationHandler.h"
#include "lld/Core/Simple.h"

namespace lld {
namespace elf {
class X86_64LinkingContext;

template <class ELFT> class X86_64TargetLayout : public TargetLayout<ELFT> {
public:
  X86_64TargetLayout(X86_64LinkingContext &context)
      : TargetLayout<ELFT>(context) {}
};

class X86_64TargetHandler final
    : public DefaultTargetHandler<X86_64ELFType> {
public:
  X86_64TargetHandler(X86_64LinkingContext &context);

  X86_64TargetLayout<X86_64ELFType> &getTargetLayout() override {
    return *(_x86_64TargetLayout.get());
  }

  void registerRelocationNames(Registry &registry) override;

  const X86_64TargetRelocationHandler &getRelocationHandler() const override {
    return *(_x86_64RelocationHandler.get());
  }

  std::unique_ptr<Reader> getObjReader(bool atomizeStrings) override {
    return std::unique_ptr<Reader>(new X86_64ELFObjectReader(atomizeStrings));
  }

  std::unique_ptr<Reader> getDSOReader(bool useShlibUndefines) override {
    return std::unique_ptr<Reader>(new X86_64ELFDSOReader(useShlibUndefines));
  }

  std::unique_ptr<Writer> getWriter() override;

private:
  static const Registry::KindStrings kindStrings[];
  X86_64LinkingContext &_context;
  std::unique_ptr<X86_64TargetLayout<X86_64ELFType>> _x86_64TargetLayout;
  std::unique_ptr<X86_64TargetRelocationHandler> _x86_64RelocationHandler;
};

} // end namespace elf
} // end namespace lld

#endif
