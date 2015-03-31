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

#include "TargetLayout.h"
#include "X86_64ELFFile.h"
#include "X86_64ELFReader.h"
#include "X86_64LinkingContext.h"
#include "X86_64RelocationHandler.h"
#include "lld/Core/Simple.h"

namespace lld {
namespace elf {
class X86_64TargetLayout : public TargetLayout<X86_64ELFType> {
public:
  X86_64TargetLayout(X86_64LinkingContext &ctx) : TargetLayout(ctx) {}

  void finalizeOutputSectionLayout() override {
    sortOutputSectionByPriority(".init_array", ".init_array");
    sortOutputSectionByPriority(".fini_array", ".fini_array");
  }
};

class X86_64TargetHandler : public TargetHandler<X86_64ELFType> {
public:
  X86_64TargetHandler(X86_64LinkingContext &ctx);

  void registerRelocationNames(Registry &registry) override;

  const X86_64TargetRelocationHandler &getRelocationHandler() const override {
    return *_x86_64RelocationHandler;
  }

  std::unique_ptr<Reader> getObjReader() override {
    return llvm::make_unique<X86_64ELFObjectReader>(_ctx);
  }

  std::unique_ptr<Reader> getDSOReader() override {
    return llvm::make_unique<X86_64ELFDSOReader>(_ctx);
  }

  std::unique_ptr<Writer> getWriter() override;

protected:
  static const Registry::KindStrings kindStrings[];
  X86_64LinkingContext &_ctx;
  std::unique_ptr<X86_64TargetLayout> _x86_64TargetLayout;
  std::unique_ptr<X86_64TargetRelocationHandler> _x86_64RelocationHandler;
};

} // end namespace elf
} // end namespace lld

#endif
