//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Chunks.h"
#include "Config.h"
#include "Error.h"
#include "SymbolTable.h"
#include "Writer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Support/FileOutputBuffer.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;

using namespace lld;
using namespace lld::elf2;

static const int PageSize = 4096;

namespace {
// OutputSection represents a section in an output file. It's a
// container of chunks. OutputSection and Chunk are 1:N relationship.
// Chunks cannot belong to more than one OutputSections. The writer
// creates multiple OutputSections and assign them unique,
// non-overlapping file offsets and VAs.
template <bool Is64Bits> class OutputSection {
public:
  typedef typename std::conditional<Is64Bits, uint64_t, uint32_t>::type uintX_t;
  typedef
      typename std::conditional<Is64Bits, Elf64_Shdr, Elf32_Shdr>::type HeaderT;

  OutputSection(StringRef Name, uint32_t sh_type, uintX_t sh_flags)
      : Name(Name) {
    memset(&Header, 0, sizeof(HeaderT));
    Header.sh_type = sh_type;
    Header.sh_flags = sh_flags;
  }
  void setVA(uintX_t VA) { Header.sh_addr = VA; }
  void setFileOffset(uintX_t Off) { Header.sh_offset = Off; }
  void addChunk(Chunk *C);
  std::vector<Chunk *> &getChunks() { return Chunks; }
  template <endianness E>
  void writeHeaderTo(typename ELFFile<ELFType<E, Is64Bits>>::Elf_Shdr *SHdr);
  StringRef getName() { return Name; }
  void setNameOffset(uintX_t Offset) { Header.sh_name = Offset; }

  // Returns the size of the section in the output file.
  uintX_t getSize() { return Header.sh_size; }
  uintX_t getFlags() { return Header.sh_flags; }
  uintX_t getOffset() { return Header.sh_offset; }

private:
  StringRef Name;
  HeaderT Header;
  std::vector<Chunk *> Chunks;
};

// The writer writes a SymbolTable result to a file.
template <class ELFT> class Writer {
public:
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  Writer(SymbolTable *T) : Symtab(T) {}
  void run();

private:
  void createSections();
  void assignAddresses();
  void openFile(StringRef OutputPath);
  void writeHeader();
  void writeSections();

  SymbolTable *Symtab;
  std::unique_ptr<llvm::FileOutputBuffer> Buffer;
  llvm::SpecificBumpPtrAllocator<OutputSection<ELFT::Is64Bits>> CAlloc;
  std::vector<OutputSection<ELFT::Is64Bits> *> OutputSections;

  uintX_t FileSize;
  uintX_t SizeOfHeaders;
  uintX_t SectionHeaderOff;
  uintX_t StringTableOff;
  unsigned StringTableIndex;
  StringTableBuilder StrTabBuilder;
  unsigned NumSections;
};
} // anonymous namespace

namespace lld {
namespace elf2 {

template <class ELFT>
void writeResult(SymbolTable *Symtab) { Writer<ELFT>(Symtab).run(); }

template void writeResult<ELF32LE>(SymbolTable *);
template void writeResult<ELF32BE>(SymbolTable *);
template void writeResult<ELF64LE>(SymbolTable *);
template void writeResult<ELF64BE>(SymbolTable *);

} // namespace elf2
} // namespace lld

// The main function of the writer.
template <class ELFT> void Writer<ELFT>::run() {
  createSections();
  assignAddresses();
  openFile(Config->OutputFile);
  writeHeader();
  writeSections();
  error(Buffer->commit());
}

template <bool Is64Bits> void OutputSection<Is64Bits>::addChunk(Chunk *C) {
  Chunks.push_back(C);
  uintX_t Off = Header.sh_size;
  Off = RoundUpToAlignment(Off, C->getAlign());
  C->setOutputSectionOff(Off);
  Off += C->getSize();
  Header.sh_size = Off;
}

template <bool Is64Bits>
template <endianness E>
void OutputSection<Is64Bits>::writeHeaderTo(
    typename ELFFile<ELFType<E, Is64Bits>>::Elf_Shdr *SHdr) {
  SHdr->sh_name = Header.sh_name;
  SHdr->sh_type = Header.sh_type;
  SHdr->sh_flags = Header.sh_flags;
  SHdr->sh_addr = Header.sh_addr;
  SHdr->sh_offset = Header.sh_offset;
  SHdr->sh_size = Header.sh_size;
  SHdr->sh_link = Header.sh_link;
  SHdr->sh_info = Header.sh_info;
  SHdr->sh_addralign = Header.sh_addralign;
  SHdr->sh_entsize = Header.sh_entsize;
}

namespace {
template <bool Is64Bits> struct SectionKey {
  typedef typename std::conditional<Is64Bits, uint64_t, uint32_t>::type uintX_t;
  StringRef Name;
  uint32_t sh_type;
  uintX_t sh_flags;
};
}
namespace llvm {
template <bool Is64Bits> struct DenseMapInfo<SectionKey<Is64Bits>> {
  static SectionKey<Is64Bits> getEmptyKey() {
    return SectionKey<Is64Bits>{DenseMapInfo<StringRef>::getEmptyKey(), 0, 0};
  }
  static SectionKey<Is64Bits> getTombstoneKey() {
    return SectionKey<Is64Bits>{DenseMapInfo<StringRef>::getTombstoneKey(), 0,
                                0};
  }
  static unsigned getHashValue(const SectionKey<Is64Bits> &Val) {
    return hash_combine(Val.Name, Val.sh_type, Val.sh_flags);
  }
  static bool isEqual(const SectionKey<Is64Bits> &LHS,
                      const SectionKey<Is64Bits> &RHS) {
    return DenseMapInfo<StringRef>::isEqual(LHS.Name, RHS.Name) &&
           LHS.sh_type == RHS.sh_type && LHS.sh_flags == RHS.sh_flags;
  }
};
}

// Create output section objects and add them to OutputSections.
template <class ELFT> void Writer<ELFT>::createSections() {
  SmallDenseMap<SectionKey<ELFT::Is64Bits>, OutputSection<ELFT::Is64Bits> *>
      Map;
  for (std::unique_ptr<ObjectFileBase> &FileB : Symtab->ObjectFiles) {
    auto &File = cast<ObjectFile<ELFT>>(*FileB);
    for (SectionChunk<ELFT> *C : File.getChunks()) {
      const Elf_Shdr *H = C->getSectionHdr();
      SectionKey<ELFT::Is64Bits> Key{C->getSectionName(), H->sh_type,
                                     H->sh_flags};
      OutputSection<ELFT::Is64Bits> *&Sec = Map[Key];
      if (!Sec) {
        Sec = new (CAlloc.Allocate())
            OutputSection<ELFT::Is64Bits>(Key.Name, Key.sh_type, Key.sh_flags);
        OutputSections.push_back(Sec);
      }
      Sec->addChunk(C);
    }
  }
}

template <bool Is64Bits>
static bool compSec(OutputSection<Is64Bits> *A, OutputSection<Is64Bits> *B) {
  // Place SHF_ALLOC sections first.
  return (A->getFlags() & SHF_ALLOC) && !(B->getFlags() & SHF_ALLOC);
}

// Visits all sections to assign incremental, non-overlapping RVAs and
// file offsets.
template <class ELFT> void Writer<ELFT>::assignAddresses() {
  SizeOfHeaders = RoundUpToAlignment(sizeof(Elf_Ehdr_Impl<ELFT>), PageSize);
  uintX_t VA = 0x1000; // The first page is kept unmapped.
  uintX_t FileOff = SizeOfHeaders;

  std::stable_sort(OutputSections.begin(), OutputSections.end(),
                   compSec<ELFT::Is64Bits>);

  for (OutputSection<ELFT::Is64Bits> *Sec : OutputSections) {
    if (Sec->getFlags() & SHF_ALLOC) {
      Sec->setVA(VA);
      VA += RoundUpToAlignment(Sec->getSize(), PageSize);
    }
    Sec->setFileOffset(FileOff);
    FileOff += RoundUpToAlignment(Sec->getSize(), 8);
    StrTabBuilder.add(Sec->getName());
  }

  // Regular sections.
  NumSections = OutputSections.size();

  // First dummy section.
  NumSections++;

  // String table.
  StrTabBuilder.add(".strtab");
  StringTableIndex = NumSections;
  StringTableOff = FileOff;
  StrTabBuilder.finalize(StringTableBuilder::ELF);
  FileOff += StrTabBuilder.data().size();
  NumSections++;

  FileOff += OffsetToAlignment(FileOff, ELFT::Is64Bits ? 8 : 4);

  // Add space for section headers.
  SectionHeaderOff = FileOff;
  FileOff += NumSections * sizeof(Elf_Shdr_Impl<ELFT>);
  FileSize = SizeOfHeaders + RoundUpToAlignment(FileOff - SizeOfHeaders, 8);
}

template <class ELFT> void Writer<ELFT>::writeHeader() {
  uint8_t *Buf = Buffer->getBufferStart();
  auto *EHdr = reinterpret_cast<Elf_Ehdr_Impl<ELFT> *>(Buf);
  EHdr->e_ident[EI_MAG0] = 0x7F;
  EHdr->e_ident[EI_MAG1] = 0x45;
  EHdr->e_ident[EI_MAG2] = 0x4C;
  EHdr->e_ident[EI_MAG3] = 0x46;
  EHdr->e_ident[EI_CLASS] = ELFT::Is64Bits ? ELFCLASS64 : ELFCLASS32;
  EHdr->e_ident[EI_DATA] = ELFT::TargetEndianness == llvm::support::little
                               ? ELFDATA2LSB
                               : ELFDATA2MSB;
  EHdr->e_ident[EI_VERSION] = EV_CURRENT;
  EHdr->e_ident[EI_OSABI] = ELFOSABI_NONE;

  EHdr->e_type = ET_EXEC;
  auto &FirstObj = cast<ObjectFile<ELFT>>(*Symtab->ObjectFiles[0]);
  EHdr->e_machine = FirstObj.getObj()->getHeader()->e_machine;
  EHdr->e_version = EV_CURRENT;
  EHdr->e_entry = 0x401000;
  EHdr->e_phoff = sizeof(Elf_Ehdr_Impl<ELFT>);
  EHdr->e_shoff = SectionHeaderOff;
  EHdr->e_ehsize = sizeof(Elf_Ehdr_Impl<ELFT>);
  EHdr->e_phentsize = sizeof(Elf_Phdr_Impl<ELFT>);
  EHdr->e_phnum = 1;
  EHdr->e_shentsize = sizeof(Elf_Shdr_Impl<ELFT>);
  EHdr->e_shnum = NumSections;
  EHdr->e_shstrndx = StringTableIndex;

  auto PHdrs = reinterpret_cast<Elf_Phdr_Impl<ELFT> *>(Buf + EHdr->e_phoff);
  PHdrs->p_type = PT_LOAD;
  PHdrs->p_flags = PF_R | PF_X;
  PHdrs->p_offset = 0x0000;
  PHdrs->p_vaddr = 0x400000;
  PHdrs->p_paddr = PHdrs->p_vaddr;
  PHdrs->p_filesz = FileSize;
  PHdrs->p_memsz = FileSize;
  PHdrs->p_align = 0x4000;

  auto SHdrs = reinterpret_cast<Elf_Shdr_Impl<ELFT> *>(Buf + EHdr->e_shoff);
  // First entry is null.
  ++SHdrs;
  for (OutputSection<ELFT::Is64Bits> *Sec : OutputSections) {
    Sec->setNameOffset(StrTabBuilder.getOffset(Sec->getName()));
    Sec->template writeHeaderTo<ELFT::TargetEndianness>(SHdrs++);
  }

  // String table.
  SHdrs->sh_name = StrTabBuilder.getOffset(".strtab");
  SHdrs->sh_type = SHT_STRTAB;
  SHdrs->sh_flags = 0;
  SHdrs->sh_addr = 0;
  SHdrs->sh_offset = StringTableOff;
  SHdrs->sh_size = StrTabBuilder.data().size();
  SHdrs->sh_link = 0;
  SHdrs->sh_info = 0;
  SHdrs->sh_addralign = 1;
  SHdrs->sh_entsize = 0;
}

template <class ELFT> void Writer<ELFT>::openFile(StringRef Path) {
  ErrorOr<std::unique_ptr<FileOutputBuffer>> BufferOrErr =
      FileOutputBuffer::create(Path, FileSize, FileOutputBuffer::F_executable);
  error(BufferOrErr, Twine("failed to open ") + Path);
  Buffer = std::move(*BufferOrErr);
}

// Write section contents to a mmap'ed file.
template <class ELFT> void Writer<ELFT>::writeSections() {
  uint8_t *Buf = Buffer->getBufferStart();
  for (OutputSection<ELFT::Is64Bits> *Sec : OutputSections) {
    uint8_t *SecBuf = Buf + Sec->getOffset();
    for (Chunk *C : Sec->getChunks())
      C->writeTo(SecBuf);
  }

  // String table.
  StringRef Data = StrTabBuilder.data();
  memcpy(Buf + StringTableOff, Data.data(), Data.size());
}
