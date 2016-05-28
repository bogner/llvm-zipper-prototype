//===- ModStream.h - PDB Module Info Stream Access ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_RAW_MODSTREAM_H
#define LLVM_DEBUGINFO_PDB_RAW_MODSTREAM_H

#include "llvm/ADT/iterator_range.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/StreamRef.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/PDB/Raw/MappedBlockStream.h"
#include "llvm/Support/Error.h"

namespace llvm {
namespace pdb {
class PDBFile;
class ModInfo;

class ModStream {
public:
  ModStream(PDBFile &File, const ModInfo &Module);
  ~ModStream();

  Error reload();

  iterator_range<codeview::CVSymbolArray::Iterator>
  symbols(bool *HadError) const;

private:
  const ModInfo &Mod;

  MappedBlockStream Stream;

  codeview::CVSymbolArray SymbolsSubstream;
  codeview::StreamRef LinesSubstream;
  codeview::StreamRef C13LinesSubstream;
  codeview::StreamRef GlobalRefsSubstream;
};
}
}

#endif
