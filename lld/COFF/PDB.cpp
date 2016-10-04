//===- PDB.cpp ------------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "PDB.h"
#include "Error.h"
#include "llvm/DebugInfo/MSF/MSFBuilder.h"
#include "llvm/DebugInfo/MSF/MSFCommon.h"
#include "llvm/DebugInfo/PDB/Raw/DbiStream.h"
#include "llvm/DebugInfo/PDB/Raw/DbiStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Raw/InfoStream.h"
#include "llvm/DebugInfo/PDB/Raw/InfoStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Raw/PDBFile.h"
#include "llvm/DebugInfo/PDB/Raw/PDBFileBuilder.h"
#include "llvm/DebugInfo/PDB/Raw/TpiStream.h"
#include "llvm/DebugInfo/PDB/Raw/TpiStreamBuilder.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include <memory>

using namespace lld;
using namespace llvm;
using namespace llvm::support;
using namespace llvm::support::endian;

static ExitOnError ExitOnErr;

void coff::createPDB(StringRef Path) {
  BumpPtrAllocator Alloc;
  pdb::PDBFileBuilder Builder(Alloc);
  ExitOnErr(Builder.initialize(4096)); // 4096 is blocksize

  ExitOnErr(Builder.getMsfBuilder().addStream(1));
  ExitOnErr(Builder.getMsfBuilder().addStream(1));
  ExitOnErr(Builder.getMsfBuilder().addStream(1));

  // Add an Info stream.
  auto &InfoBuilder = Builder.getInfoBuilder();
  InfoBuilder.setAge(1);

  // Should be a random number, 0 for now.
  InfoBuilder.setGuid({});

  // Should be the current time, but set 0 for reproducibilty.
  InfoBuilder.setSignature(0);

  InfoBuilder.setVersion(pdb::PdbRaw_ImplVer::PdbImplVC70);

  // Add an empty TPI stream.
  auto &TpiBuilder = Builder.getTpiBuilder();
  TpiBuilder.setVersionHeader(pdb::PdbTpiV80);

  // Write to a file.
  ExitOnErr(Builder.commit(Path));
}
