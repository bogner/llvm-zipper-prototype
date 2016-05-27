//===- MappedBlockStream.cpp - Reads stream data from a PDBFile -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/Raw/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/Raw/PDBFile.h"
#include "llvm/DebugInfo/PDB/Raw/RawError.h"

using namespace llvm;
using namespace llvm::pdb;

MappedBlockStream::MappedBlockStream(uint32_t StreamIdx, const PDBFile &File) : Pdb(File) {
  if (StreamIdx >= Pdb.getNumStreams()) {
    StreamLength = 0;
  } else {
    StreamLength = Pdb.getStreamByteSize(StreamIdx);
    BlockList = Pdb.getStreamBlockList(StreamIdx);
  }
}

Error MappedBlockStream::readBytes(uint32_t Offset, uint32_t Size,
                                   ArrayRef<uint8_t> &Buffer) const {
  // Make sure we aren't trying to read beyond the end of the stream.
  if (Size > StreamLength)
    return make_error<RawError>(raw_error_code::insufficient_buffer);
  if (Offset > StreamLength - Size)
    return make_error<RawError>(raw_error_code::insufficient_buffer);

  if (tryReadContiguously(Offset, Size, Buffer))
    return Error::success();

  auto CacheIter = CacheMap.find(Offset);
  if (CacheIter != CacheMap.end()) {
    // In a more general solution, we would need to guarantee that the
    // cached allocation is at least the requested size.  In practice, since
    // these are CodeView / PDB records, we know they are always formatted
    // the same way and never change, so we should never be requesting two
    // allocations from the same address with different sizes.
    Buffer = ArrayRef<uint8_t>(CacheIter->second, Size);
    return Error::success();
  }

  // Otherwise allocate a large enough buffer in the pool, memcpy the data
  // into it, and return an ArrayRef to that.
  uint8_t *WriteBuffer = Pool.Allocate<uint8_t>(Size);

  if (auto EC = readBytes(Offset, MutableArrayRef<uint8_t>(WriteBuffer, Size)))
    return EC;
  CacheMap.insert(std::make_pair(Offset, WriteBuffer));
  Buffer = ArrayRef<uint8_t>(WriteBuffer, Size);
  return Error::success();
}

bool MappedBlockStream::tryReadContiguously(uint32_t Offset, uint32_t Size,
                                            ArrayRef<uint8_t> &Buffer) const {
  // Attempt to fulfill the request with a reference directly into the stream.
  // This can work even if the request crosses a block boundary, provided that
  // all subsequent blocks are contiguous.  For example, a 10k read with a 4k
  // block size can be filled with a reference if, from the starting offset,
  // 3 blocks in a row are contiguous.
  uint32_t BlockNum = Offset / Pdb.getBlockSize();
  uint32_t OffsetInBlock = Offset % Pdb.getBlockSize();
  uint32_t BytesFromFirstBlock =
      std::min(Size, Pdb.getBlockSize() - OffsetInBlock);
  uint32_t NumAdditionalBlocks =
      llvm::alignTo(Size - BytesFromFirstBlock, Pdb.getBlockSize()) /
      Pdb.getBlockSize();

  uint32_t RequiredContiguousBlocks = NumAdditionalBlocks + 1;
  uint32_t E = BlockList[BlockNum];
  for (uint32_t I = 0; I < RequiredContiguousBlocks; ++I, ++E) {
    if (BlockList[I + BlockNum] != E)
      return false;
  }

  uint32_t FirstBlockAddr = BlockList[BlockNum];
  StringRef Str = Pdb.getBlockData(FirstBlockAddr, Pdb.getBlockSize());
  Str = Str.drop_front(OffsetInBlock);
  Buffer =
      ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Str.data()), Size);
  return true;
}

Error MappedBlockStream::readBytes(uint32_t Offset,
                                   MutableArrayRef<uint8_t> Buffer) const {
  uint32_t BlockNum = Offset / Pdb.getBlockSize();
  uint32_t OffsetInBlock = Offset % Pdb.getBlockSize();

  // Make sure we aren't trying to read beyond the end of the stream.
  if (Buffer.size() > StreamLength)
    return make_error<RawError>(raw_error_code::insufficient_buffer);
  if (Offset > StreamLength - Buffer.size())
    return make_error<RawError>(raw_error_code::insufficient_buffer);

  uint32_t BytesLeft = Buffer.size();
  uint32_t BytesWritten = 0;
  uint8_t *WriteBuffer = Buffer.data();
  while (BytesLeft > 0) {
    uint32_t StreamBlockAddr = BlockList[BlockNum];

    StringRef Data = Pdb.getBlockData(StreamBlockAddr, Pdb.getBlockSize());

    const char *ChunkStart = Data.data() + OffsetInBlock;
    uint32_t BytesInChunk =
        std::min(BytesLeft, Pdb.getBlockSize() - OffsetInBlock);
    ::memcpy(WriteBuffer + BytesWritten, ChunkStart, BytesInChunk);

    BytesWritten += BytesInChunk;
    BytesLeft -= BytesInChunk;
    ++BlockNum;
    OffsetInBlock = 0;
  }

  return Error::success();

}
