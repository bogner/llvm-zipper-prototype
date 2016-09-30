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
  // Create the superblock.
  msf::SuperBlock SB;
  memcpy(SB.MagicBytes, msf::Magic, sizeof(msf::Magic));
  SB.BlockSize = 4096;
  SB.FreeBlockMapBlock = 2;
  SB.NumBlocks = 10;
  SB.NumDirectoryBytes = 0;
  SB.Unknown1 = 0;
  SB.BlockMapAddr = 9;

  BumpPtrAllocator Alloc;
  pdb::PDBFileBuilder Builder(Alloc);
  ExitOnErr(Builder.initialize(SB));
  ExitOnErr(Builder.getMsfBuilder().setDirectoryBlocksHint({8}));

  ExitOnErr(Builder.getMsfBuilder().addStream(1, {4}));
  ExitOnErr(Builder.getMsfBuilder().addStream(1, {5}));
  ExitOnErr(Builder.getMsfBuilder().addStream(1, {6}));

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
