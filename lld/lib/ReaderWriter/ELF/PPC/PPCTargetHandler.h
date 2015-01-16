//===- lib/ReaderWriter/ELF/PPC/PPCTargetHandler.h ------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_PPC_PPC_TARGET_HANDLER_H
#define LLD_READER_WRITER_ELF_PPC_PPC_TARGET_HANDLER_H

#include "DefaultTargetHandler.h"
#include "PPCELFReader.h"
#include "TargetLayout.h"

namespace lld {
namespace elf {
class PPCLinkingContext;

template <class ELFT> class PPCTargetLayout : public TargetLayout<ELFT> {
public:
  PPCTargetLayout(PPCLinkingContext &context) : TargetLayout<ELFT>(context) {}
};

class PPCTargetRelocationHandler final
    : public TargetRelocationHandler<PPCELFType> {
public:
  PPCTargetRelocationHandler(ELFLinkingContext &context)
      : TargetRelocationHandler<PPCELFType>(context) {}

  virtual std::error_code applyRelocation(ELFWriter &, llvm::FileOutputBuffer &,
                                          const lld::AtomLayout &,
                                          const Reference &) const override;
};

class PPCTargetHandler final
    : public DefaultTargetHandler<PPCELFType> {
public:
  PPCTargetHandler(PPCLinkingContext &context);

  PPCTargetLayout<PPCELFType> &getTargetLayout() override {
    return *(_ppcTargetLayout.get());
  }

  void registerRelocationNames(Registry &registry) override;

  const PPCTargetRelocationHandler &getRelocationHandler() const override {
    return *(_ppcRelocationHandler.get());
  }

  std::unique_ptr<Reader> getObjReader(bool atomizeStrings) override {
    return std::unique_ptr<Reader>(new PPCELFObjectReader(atomizeStrings));
  }

  std::unique_ptr<Reader> getDSOReader(bool useShlibUndefines) override {
    return std::unique_ptr<Reader>(new PPCELFDSOReader(useShlibUndefines));
  }

  std::unique_ptr<Writer> getWriter() override;

private:
  static const Registry::KindStrings kindStrings[];
  PPCLinkingContext &_ppcLinkingContext;
  std::unique_ptr<PPCTargetLayout<PPCELFType>> _ppcTargetLayout;
  std::unique_ptr<PPCTargetRelocationHandler> _ppcRelocationHandler;
};
} // end namespace elf
} // end namespace lld

#endif
