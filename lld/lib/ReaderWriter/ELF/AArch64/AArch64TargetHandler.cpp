//===- lib/ReaderWriter/ELF/AArch64/AArch64TargetHandler.cpp --------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Atoms.h"
#include "AArch64DynamicLibraryWriter.h"
#include "AArch64ExecutableWriter.h"
#include "AArch64LinkingContext.h"
#include "AArch64TargetHandler.h"

using namespace lld;
using namespace elf;

AArch64TargetHandler::AArch64TargetHandler(AArch64LinkingContext &ctx)
    : _ctx(ctx), _targetLayout(new TargetLayout<AArch64ELFType>(ctx)),
      _relocationHandler(new AArch64TargetRelocationHandler()) {}

static const Registry::KindStrings kindStrings[] = {
#define ELF_RELOC(name, value) LLD_KIND_STRING_ENTRY(name),
#include "llvm/Support/ELFRelocs/AArch64.def"
#undef ELF_RELOC
  LLD_KIND_STRING_END
};

void AArch64TargetHandler::registerRelocationNames(Registry &registry) {
  registry.addKindTable(Reference::KindNamespace::ELF,
                        Reference::KindArch::AArch64, kindStrings);
}

std::unique_ptr<Writer> AArch64TargetHandler::getWriter() {
  switch (this->_ctx.getOutputELFType()) {
  case llvm::ELF::ET_EXEC:
    return llvm::make_unique<AArch64ExecutableWriter<AArch64ELFType>>(
        _ctx, *_targetLayout);
  case llvm::ELF::ET_DYN:
    return llvm::make_unique<AArch64DynamicLibraryWriter<AArch64ELFType>>(
        _ctx, *_targetLayout);
  case llvm::ELF::ET_REL:
    llvm_unreachable("TODO: support -r mode");
  default:
    llvm_unreachable("unsupported output type");
  }
}
