//===- lib/ReaderWriter/PECOFF/PDBPass.h ----------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_PE_COFF_PDB_PASS_H
#define LLD_READER_WRITER_PE_COFF_PDB_PASS_H

#include "lld/Core/Pass.h"
#include "llvm/ADT/StringRef.h"

namespace lld {
namespace pecoff {

class PDBPass : public lld::Pass {
public:
  PDBPass(PECOFFLinkingContext &ctx) : _ctx(ctx) {}

  void perform(std::unique_ptr<MutableFile> &file) override {
    if (_ctx.getDebug())
      touch(_ctx.getPDBFilePath());
  }

private:
  void touch(StringRef path) {
    int fd;
    if (llvm::sys::fs::openFileForWrite(path, fd, llvm::sys::fs::F_Append))
      llvm::report_fatal_error("failed to create a PDB file");
  }

  PECOFFLinkingContext &_ctx;
};

} // namespace pecoff
} // namespace lld

#endif
