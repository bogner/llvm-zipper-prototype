//===- IPDBFile.h - Abstract base class for a PDB file ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_RAW_IPDBFILE_H
#define LLVM_DEBUGINFO_PDB_RAW_IPDBFILE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <stdint.h>

namespace llvm {
namespace pdb {

struct IPDBFile {
public:
  virtual ~IPDBFile() {}

  virtual uint32_t getBlockSize() const = 0;
  virtual uint32_t getBlockCount() const = 0;
  virtual uint32_t getNumDirectoryBytes() const = 0;
  virtual uint32_t getBlockMapIndex() const = 0;
  virtual uint32_t getNumDirectoryBlocks() const = 0;
  virtual uint64_t getBlockMapOffset() const = 0;

  virtual uint32_t getNumStreams() const = 0;
  virtual uint32_t getStreamByteSize(uint32_t StreamIndex) const = 0;
  virtual ArrayRef<uint32_t> getStreamBlockList(uint32_t StreamIndex) const = 0;

  virtual StringRef getBlockData(uint32_t BlockIndex,
                                 uint32_t NumBytes) const = 0;

  virtual ArrayRef<support::ulittle32_t> getDirectoryBlockArray() = 0;
};
}
}

#endif // LLVM_DEBUGINFO_PDB_RAW_IPDBFILE_H
