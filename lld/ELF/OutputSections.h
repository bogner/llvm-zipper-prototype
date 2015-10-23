//===- OutputSections.h -----------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_OUTPUT_SECTIONS_H
#define LLD_ELF_OUTPUT_SECTIONS_H

#include "lld/Core/LLVM.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Object/ELF.h"

#include "Config.h"

#include <type_traits>

namespace lld {
namespace elf2 {

class SymbolBody;
template <class ELFT> class SymbolTable;
template <class ELFT> class SymbolTableSection;
template <class ELFT> class StringTableSection;
template <class ELFT> class InputSection;
template <class ELFT> class MergeInputSection;
template <class ELFT> class OutputSection;
template <class ELFT> class ObjectFile;
template <class ELFT> class DefinedRegular;
template <class ELFT> class ELFSymbolBody;

template <class ELFT>
static inline typename llvm::object::ELFFile<ELFT>::uintX_t
getAddend(const typename llvm::object::ELFFile<ELFT>::Elf_Rel &Rel) {
  return 0;
}

template <class ELFT>
static inline typename llvm::object::ELFFile<ELFT>::uintX_t
getAddend(const typename llvm::object::ELFFile<ELFT>::Elf_Rela &Rel) {
  return Rel.r_addend;
}

template <class ELFT>
typename llvm::object::ELFFile<ELFT>::uintX_t getSymVA(const SymbolBody &S);

template <class ELFT, bool IsRela>
typename llvm::object::ELFFile<ELFT>::uintX_t
getLocalRelTarget(const ObjectFile<ELFT> &File,
                  const llvm::object::Elf_Rel_Impl<ELFT, IsRela> &Rel);
bool canBePreempted(const SymbolBody *Body, bool NeedsGot);
template <class ELFT> bool includeInSymtab(const SymbolBody &B);

bool includeInDynamicSymtab(const SymbolBody &B);
bool includeInGnuHashTable(const SymbolBody &B);

template <class ELFT>
bool shouldKeepInSymtab(
    const ObjectFile<ELFT> &File, StringRef Name,
    const typename llvm::object::ELFFile<ELFT>::Elf_Sym &Sym);

// This represents a section in an output file.
// Different sub classes represent different types of sections. Some contain
// input sections, others are created by the linker.
// The writer creates multiple OutputSections and assign them unique,
// non-overlapping file offsets and VAs.
template <class ELFT> class OutputSectionBase {
public:
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Shdr Elf_Shdr;

  OutputSectionBase(StringRef Name, uint32_t sh_type, uintX_t sh_flags);
  void setVA(uintX_t VA) { Header.sh_addr = VA; }
  uintX_t getVA() const { return Header.sh_addr; }
  void setFileOffset(uintX_t Off) { Header.sh_offset = Off; }
  void writeHeaderTo(Elf_Shdr *SHdr);
  StringRef getName() { return Name; }
  void setNameOffset(uintX_t Offset) { Header.sh_name = Offset; }

  unsigned SectionIndex;

  // Returns the size of the section in the output file.
  uintX_t getSize() const { return Header.sh_size; }
  void setSize(uintX_t Val) { Header.sh_size = Val; }
  uintX_t getFlags() { return Header.sh_flags; }
  uintX_t getFileOff() { return Header.sh_offset; }
  uintX_t getAlign() {
    // The ELF spec states that a value of 0 means the section has no alignment
    // constraits.
    return std::max<uintX_t>(Header.sh_addralign, 1);
  }
  uint32_t getType() { return Header.sh_type; }

  virtual void finalize() {}
  virtual void writeTo(uint8_t *Buf) = 0;

protected:
  StringRef Name;
  Elf_Shdr Header;
  ~OutputSectionBase() = default;
};

template <class ELFT> class GotSection final : public OutputSectionBase<ELFT> {
  typedef OutputSectionBase<ELFT> Base;
  typedef typename Base::uintX_t uintX_t;

public:
  GotSection();
  void finalize() override {
    this->Header.sh_size = Entries.size() * sizeof(uintX_t);
  }
  void writeTo(uint8_t *Buf) override;
  void addEntry(SymbolBody *Sym);
  bool empty() const { return Entries.empty(); }
  uintX_t getEntryAddr(const SymbolBody &B) const;

private:
  std::vector<const SymbolBody *> Entries;
};

template <class ELFT>
class GotPltSection final : public OutputSectionBase<ELFT> {
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;

public:
  GotPltSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  void addEntry(SymbolBody *Sym);
  bool empty() const;
  uintX_t getEntryAddr(const SymbolBody &B) const;

private:
  std::vector<const SymbolBody *> Entries;
};

template <class ELFT> class PltSection final : public OutputSectionBase<ELFT> {
  typedef OutputSectionBase<ELFT> Base;
  typedef typename Base::uintX_t uintX_t;

public:
  PltSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  void addEntry(SymbolBody *Sym);
  bool empty() const { return Entries.empty(); }
  uintX_t getEntryAddr(const SymbolBody &B) const;

private:
  std::vector<const SymbolBody *> Entries;
};

template <class ELFT> struct DynamicReloc {
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel Elf_Rel;
  const InputSection<ELFT> &C;
  const Elf_Rel &RI;
};

template <class ELFT>
class SymbolTableSection final : public OutputSectionBase<ELFT> {
public:
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Sym Elf_Sym;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Sym_Range Elf_Sym_Range;
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  SymbolTableSection(SymbolTable<ELFT> &Table,
                     StringTableSection<ELFT> &StrTabSec);

  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  void addLocalSymbol(StringRef Name);
  void addSymbol(SymbolBody *Body);
  StringTableSection<ELFT> &getStrTabSec() const { return StrTabSec; }
  unsigned getNumSymbols() const { return NumVisible + 1; }
  unsigned getNumGnuHashSymbols() const { return NumGnuHashed; }

  struct SymbolData {
    SymbolData(SymbolBody *Body, bool HasGnuHash);
    SymbolBody *Body;
    bool HasGnuHash;
    uint32_t GnuHash;
  };
  ArrayRef<SymbolData> getSymbols() const { return Symbols; }
  ArrayRef<SymbolData> getGnuHashSymbols() const;

private:
  void writeLocalSymbols(uint8_t *&Buf);
  void writeGlobalSymbols(uint8_t *Buf);

  static uint8_t getSymbolBinding(SymbolBody *Body);

  SymbolTable<ELFT> &Table;
  StringTableSection<ELFT> &StrTabSec;
  std::vector<SymbolData> Symbols;
  unsigned NumVisible = 0;
  unsigned NumLocals = 0;
  unsigned NumGnuHashed = 0;
};

template <class ELFT>
class RelocationSection final : public OutputSectionBase<ELFT> {
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel Elf_Rel;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rela Elf_Rela;
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;

public:
  RelocationSection(StringRef Name, bool IsRela);
  void addReloc(const DynamicReloc<ELFT> &Reloc) { Relocs.push_back(Reloc); }
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  bool hasRelocs() const { return !Relocs.empty(); }
  bool isRela() const { return IsRela; }

private:
  std::vector<DynamicReloc<ELFT>> Relocs;
  const bool IsRela;
};

template <class ELFT>
class OutputSection final : public OutputSectionBase<ELFT> {
public:
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Sym Elf_Sym;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel Elf_Rel;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rela Elf_Rela;
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  OutputSection(StringRef Name, uint32_t sh_type, uintX_t sh_flags);
  void addSection(InputSection<ELFT> *C);
  void writeTo(uint8_t *Buf) override;

private:
  std::vector<InputSection<ELFT> *> Sections;
};

template <class ELFT>
class MergeOutputSection final : public OutputSectionBase<ELFT> {
  typedef typename OutputSectionBase<ELFT>::uintX_t uintX_t;

public:
  MergeOutputSection(StringRef Name, uint32_t sh_type, uintX_t sh_flags);
  void addSection(MergeInputSection<ELFT> *S);
  void writeTo(uint8_t *Buf) override;

  unsigned getOffset(ArrayRef<uint8_t> Val);

private:
  // This map is used to find if we already have an entry for a given value and,
  // if so, at what offset it is.
  llvm::MapVector<ArrayRef<uint8_t>, uintX_t> Offsets;
};

template <class ELFT>
class InterpSection final : public OutputSectionBase<ELFT> {
public:
  InterpSection();
  void writeTo(uint8_t *Buf);
};

template <class ELFT>
class StringTableSection final : public OutputSectionBase<ELFT> {
public:
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  StringTableSection(StringRef Name, bool Dynamic);
  void add(StringRef S) { StrTabBuilder.add(S); }
  size_t getFileOff(StringRef S) const { return StrTabBuilder.getOffset(S); }
  StringRef data() const { return StrTabBuilder.data(); }
  void writeTo(uint8_t *Buf) override;

  void finalize() override {
    StrTabBuilder.finalize();
    this->Header.sh_size = StrTabBuilder.data().size();
  }

  bool isDynamic() const { return Dynamic; }

private:
  const bool Dynamic;
  llvm::StringTableBuilder StrTabBuilder{llvm::StringTableBuilder::ELF};
};

template <class ELFT>
class HashTableSection final : public OutputSectionBase<ELFT> {
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Word Elf_Word;

public:
  HashTableSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
};

// Outputs GNU Hash section. For detailed explanation see:
// https://blogs.oracle.com/ali/entry/gnu_hash_elf_sections
template <class ELFT>
class GnuHashTableSection final : public OutputSectionBase<ELFT> {
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Off Elf_Off;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Word Elf_Word;
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;

public:
  GnuHashTableSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;

  static unsigned calcNBuckets(unsigned NumHashed);

private:
  static unsigned calcMaskWords(unsigned NumHashed);

  void writeHeader(uint8_t *&Buf);
  void writeBloomFilter(uint8_t *&Buf);
  void writeHashTable(uint8_t *Buf);

  unsigned MaskWords;
  unsigned NBuckets;
  unsigned Shift2;
};

template <class ELFT>
class DynamicSection final : public OutputSectionBase<ELFT> {
  typedef OutputSectionBase<ELFT> Base;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Dyn Elf_Dyn;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel Elf_Rel;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rela Elf_Rela;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Sym Elf_Sym;

public:
  DynamicSection(SymbolTable<ELFT> &SymTab);
  void finalize() override;
  void writeTo(uint8_t *Buf) override;

  OutputSectionBase<ELFT> *PreInitArraySec = nullptr;
  OutputSectionBase<ELFT> *InitArraySec = nullptr;
  OutputSectionBase<ELFT> *FiniArraySec = nullptr;

private:
  SymbolTable<ELFT> &SymTab;
  const ELFSymbolBody<ELFT> *InitSym = nullptr;
  const ELFSymbolBody<ELFT> *FiniSym = nullptr;
  uint32_t DtFlags = 0;
  uint32_t DtFlags1 = 0;
};

// All output sections that are hadnled by the linker specially are
// globally accessible. Writer initializes them, so don't use them
// until Writer is initialized.
template <class ELFT> struct Out {
  static DynamicSection<ELFT> *Dynamic;
  static GnuHashTableSection<ELFT> *GnuHashTab;
  static GotPltSection<ELFT> *GotPlt;
  static GotSection<ELFT> *Got;
  static HashTableSection<ELFT> *HashTab;
  static InterpSection<ELFT> *Interp;
  static OutputSection<ELFT> *Bss;
  static OutputSectionBase<ELFT> *Opd;
  static uint8_t *OpdBuf;
  static PltSection<ELFT> *Plt;
  static RelocationSection<ELFT> *RelaDyn;
  static RelocationSection<ELFT> *RelaPlt;
  static StringTableSection<ELFT> *DynStrTab;
  static StringTableSection<ELFT> *ShStrTab;
  static StringTableSection<ELFT> *StrTab;
  static SymbolTableSection<ELFT> *DynSymTab;
  static SymbolTableSection<ELFT> *SymTab;
};

template <class ELFT> DynamicSection<ELFT> *Out<ELFT>::Dynamic;
template <class ELFT> GnuHashTableSection<ELFT> *Out<ELFT>::GnuHashTab;
template <class ELFT> GotPltSection<ELFT> *Out<ELFT>::GotPlt;
template <class ELFT> GotSection<ELFT> *Out<ELFT>::Got;
template <class ELFT> HashTableSection<ELFT> *Out<ELFT>::HashTab;
template <class ELFT> InterpSection<ELFT> *Out<ELFT>::Interp;
template <class ELFT> OutputSection<ELFT> *Out<ELFT>::Bss;
template <class ELFT> OutputSectionBase<ELFT> *Out<ELFT>::Opd;
template <class ELFT> uint8_t *Out<ELFT>::OpdBuf;
template <class ELFT> PltSection<ELFT> *Out<ELFT>::Plt;
template <class ELFT> RelocationSection<ELFT> *Out<ELFT>::RelaDyn;
template <class ELFT> RelocationSection<ELFT> *Out<ELFT>::RelaPlt;
template <class ELFT> StringTableSection<ELFT> *Out<ELFT>::DynStrTab;
template <class ELFT> StringTableSection<ELFT> *Out<ELFT>::ShStrTab;
template <class ELFT> StringTableSection<ELFT> *Out<ELFT>::StrTab;
template <class ELFT> SymbolTableSection<ELFT> *Out<ELFT>::DynSymTab;
template <class ELFT> SymbolTableSection<ELFT> *Out<ELFT>::SymTab;
}
}
#endif
