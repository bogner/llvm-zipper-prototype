//===- Writer.h -----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_WRITER_H
#define LLD_ELF_WRITER_H

#include "SymbolTable.h"
#include "llvm/Support/FileOutputBuffer.h"

namespace lld {
namespace elf2 {
// OutputSection represents a section in an output file. It's a
// container of chunks. OutputSection and Chunk are 1:N relationship.
// Chunks cannot belong to more than one OutputSections. The writer
// creates multiple OutputSections and assign them unique,
// non-overlapping file offsets and VAs.
class OutputSection {
public:
  OutputSection(StringRef Name) : Name(Name), Header({}) {}
  void setVA(uint64_t);
  void setFileOffset(uint64_t);
  void addChunk(Chunk *C);
  std::vector<Chunk *> &getChunks() { return Chunks; }

  // Returns the size of the section in the output file.
  uint64_t getSize() { return Header.sh_size; }

  // Set offset into the string table storing this section name.
  // Used only when the name is longer than 8 bytes.
  void setStringTableOff(uint32_t V) { StringTableOff = V; }

private:
  StringRef Name;
  llvm::ELF::Elf64_Shdr Header;
  uint32_t StringTableOff = 0;
  std::vector<Chunk *> Chunks;
};

// The writer writes a SymbolTable result to a file.
template <class ELFT> class Writer {
public:
  explicit Writer(SymbolTable<ELFT> *T);
  ~Writer();
  void write(StringRef Path);

private:
  void createSections();
  void assignAddresses();
  void openFile(StringRef OutputPath);
  void writeHeader();
  void writeSections();

  SymbolTable<ELFT> *Symtab;
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  llvm::SpecificBumpPtrAllocator<OutputSection> CAlloc;
  std::vector<OutputSection *> OutputSections;

  uint64_t FileSize;
  uint64_t SizeOfImage;
  uint64_t SizeOfHeaders;

  std::vector<std::unique_ptr<Chunk>> Chunks;
};

} // namespace elf2
} // namespace lld

#endif
