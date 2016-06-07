//===- PDBFile.h - Low level interface to a PDB file ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_RAW_PDBFILE_H
#define LLVM_DEBUGINFO_PDB_RAW_PDBFILE_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/DebugInfo/CodeView/StreamArray.h"
#include "llvm/DebugInfo/PDB/Raw/IPDBFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MathExtras.h"

#include <memory>

namespace llvm {
class MemoryBuffer;

namespace pdb {
struct PDBFileContext;
class DbiStream;
class InfoStream;
class MappedBlockStream;
class NameHashTable;
class PublicsStream;
class SymbolStream;
class TpiStream;

class PDBFile : public IPDBFile {
public:
  explicit PDBFile(std::unique_ptr<MemoryBuffer> MemBuffer);
  ~PDBFile() override;

  uint32_t getUnknown0() const;
  uint32_t getUnknown1() const;

  uint32_t getBlockSize() const override;
  uint32_t getBlockCount() const override;
  uint32_t getNumDirectoryBytes() const;
  uint32_t getBlockMapIndex() const;
  uint32_t getNumDirectoryBlocks() const;
  uint64_t getBlockMapOffset() const;

  uint32_t getNumStreams() const override;
  uint32_t getStreamByteSize(uint32_t StreamIndex) const override;
  ArrayRef<support::ulittle32_t>
  getStreamBlockList(uint32_t StreamIndex) const override;

  ArrayRef<uint8_t> getBlockData(uint32_t BlockIndex,
                                 uint32_t NumBytes) const override;

  ArrayRef<support::ulittle32_t> getDirectoryBlockArray() const;

  Error parseFileHeaders();
  Error parseStreamData();

  static uint64_t bytesToBlocks(uint64_t NumBytes, uint64_t BlockSize) {
    return alignTo(NumBytes, BlockSize) / BlockSize;
  }

  static uint64_t blockToOffset(uint64_t BlockNumber, uint64_t BlockSize) {
    return BlockNumber * BlockSize;
  }

  Expected<InfoStream &> getPDBInfoStream();
  Expected<DbiStream &> getPDBDbiStream();
  Expected<TpiStream &> getPDBTpiStream();
  Expected<TpiStream &> getPDBIpiStream();
  Expected<PublicsStream &> getPDBPublicsStream();
  Expected<SymbolStream &> getPDBSymbolStream();
  Expected<NameHashTable &> getStringTable();

private:
  std::unique_ptr<PDBFileContext> Context;
  std::unique_ptr<InfoStream> Info;
  std::unique_ptr<DbiStream> Dbi;
  std::unique_ptr<TpiStream> Tpi;
  std::unique_ptr<TpiStream> Ipi;
  std::unique_ptr<PublicsStream> Publics;
  std::unique_ptr<SymbolStream> Symbols;
  std::unique_ptr<MappedBlockStream> DirectoryStream;
  std::unique_ptr<MappedBlockStream> StringTableStream;
  std::unique_ptr<NameHashTable> StringTable;
};
}
}

#endif
