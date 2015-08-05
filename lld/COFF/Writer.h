//===- Writer.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_WRITER_H
#define LLD_COFF_WRITER_H

#include <vector>

namespace lld {
namespace coff {

class Chunk;
class OutputSection;

std::error_code writeResult(SymbolTable *T, StringRef Path);

// Implemented in ICF.cpp.
void doICF(const std::vector<Chunk *> &Chunks);

}
}

#endif
