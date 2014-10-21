//===- lib/ReaderWriter/ELF/DefaultTargetHandler.h ------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_DEFAULT_TARGET_HANDLER_H
#define LLD_READER_WRITER_ELF_DEFAULT_TARGET_HANDLER_H

#include "DefaultLayout.h"
#include "DynamicLibraryWriter.h"
#include "ELFReader.h"
#include "ExecutableWriter.h"
#include "TargetHandler.h"
#include "lld/ReaderWriter/ELFLinkingContext.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/ELF.h"

namespace lld {
namespace elf {
template <class ELFT>
class DefaultTargetHandler : public TargetHandler<ELFT> {
public:
  DefaultTargetHandler(ELFLinkingContext &context)
      : TargetHandler<ELFT>(context) {}

  const TargetRelocationHandler<ELFT> &getRelocationHandler() const = 0;

  virtual std::unique_ptr<Reader> getObjReader(bool atomizeStrings) = 0;

  virtual std::unique_ptr<Reader> getDSOReader(bool useShlibUndefines) = 0;

  virtual std::unique_ptr<Writer> getWriter() = 0;
};

} // end namespace elf
} // end namespace lld
#endif
