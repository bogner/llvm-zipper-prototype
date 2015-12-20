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
template <class ELFT> class EHInputSection;
template <class ELFT> class InputSection;
template <class ELFT> class InputSectionBase;
template <class ELFT> class MergeInputSection;
template <class ELFT> class MipsReginfoInputSection;
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
  void updateAlign(uintX_t Align) {
    if (Align > Header.sh_addralign)
      Header.sh_addralign = Align;
  }

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
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  void addEntry(SymbolBody *Sym);
  bool addDynTlsEntry(SymbolBody *Sym);
  bool addCurrentModuleTlsIndex();
  bool empty() const { return Entries.empty(); }
  uintX_t getEntryAddr(const SymbolBody &B) const;
  uintX_t getGlobalDynAddr(const SymbolBody &B) const;
  uintX_t getNumEntries() const { return Entries.size(); }

  // Returns the symbol which corresponds to the first entry of the global part
  // of GOT on MIPS platform. It is required to fill up MIPS-specific dynamic
  // table properties.
  // Returns nullptr if the global part is empty.
  const SymbolBody *getMipsFirstGlobalEntry() const;

  // Returns the number of entries in the local part of GOT including
  // the number of reserved entries. This method is MIPS-specific.
  unsigned getMipsLocalEntriesNum() const;

  uint32_t getLocalTlsIndexVA() { return Base::getVA() + LocalTlsIndexOff; }

private:
  std::vector<const SymbolBody *> Entries;
  uint32_t LocalTlsIndexOff = -1;
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
  std::vector<std::pair<const SymbolBody *, unsigned>> Entries;
};

template <class ELFT> struct DynamicReloc {
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel Elf_Rel;
  InputSectionBase<ELFT> *C;
  const Elf_Rel *RI;
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

  ArrayRef<SymbolBody *> getSymbols() const { return Symbols; }

private:
  void writeLocalSymbols(uint8_t *&Buf);
  void writeGlobalSymbols(uint8_t *Buf);

  static uint8_t getSymbolBinding(SymbolBody *Body);

  SymbolTable<ELFT> &Table;
  StringTableSection<ELFT> &StrTabSec;
  std::vector<SymbolBody *> Symbols;
  unsigned NumVisible = 0;
  unsigned NumLocals = 0;
};

template <class ELFT>
class RelocationSection final : public OutputSectionBase<ELFT> {
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel Elf_Rel;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rela Elf_Rela;
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;

public:
  RelocationSection(StringRef Name, bool IsRela);
  void addReloc(const DynamicReloc<ELFT> &Reloc) { Relocs.push_back(Reloc); }
  unsigned getRelocOffset();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  bool hasRelocs() const { return !Relocs.empty(); }
  bool isRela() const { return IsRela; }

private:
  bool applyTlsDynamicReloc(SymbolBody *Body, uint32_t Type, Elf_Rel *P,
                            Elf_Rel *N);

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

  bool shouldTailMerge() const;

public:
  MergeOutputSection(StringRef Name, uint32_t sh_type, uintX_t sh_flags);
  void addSection(MergeInputSection<ELFT> *S);
  void writeTo(uint8_t *Buf) override;
  unsigned getOffset(StringRef Val);
  void finalize() override;

private:
  llvm::StringTableBuilder Builder{llvm::StringTableBuilder::RAW};
};

// FDE or CIE
template <class ELFT> struct EHRegion {
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  EHRegion(EHInputSection<ELFT> *S, unsigned Index);
  StringRef data() const;
  EHInputSection<ELFT> *S;
  unsigned Index;
};

template <class ELFT> struct Cie : public EHRegion<ELFT> {
  Cie(EHInputSection<ELFT> *S, unsigned Index);
  std::vector<EHRegion<ELFT>> Fdes;
};

template <class ELFT>
class EHOutputSection final : public OutputSectionBase<ELFT> {
public:
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel Elf_Rel;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rela Elf_Rela;
  EHOutputSection(StringRef Name, uint32_t sh_type, uintX_t sh_flags);
  void writeTo(uint8_t *Buf) override;

  template <bool IsRela>
  void addSectionAux(
      EHInputSection<ELFT> *S,
      llvm::iterator_range<const llvm::object::Elf_Rel_Impl<ELFT, IsRela> *>
          Rels);

  void addSection(EHInputSection<ELFT> *S);

private:
  uintX_t readEntryLength(ArrayRef<uint8_t> D);

  std::vector<EHInputSection<ELFT> *> Sections;
  std::vector<Cie<ELFT>> Cies;

  // Maps CIE content + personality to a index in Cies.
  llvm::DenseMap<std::pair<StringRef, StringRef>, unsigned> CieMap;
};

template <class ELFT>
class InterpSection final : public OutputSectionBase<ELFT> {
public:
  InterpSection();
  void writeTo(uint8_t *Buf) override;
};

template <class ELFT>
class StringTableSection final : public OutputSectionBase<ELFT> {
public:
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  StringTableSection(StringRef Name, bool Dynamic);
  void add(StringRef S) { StrTabBuilder.add(S); }
  size_t getOffset(StringRef S) const { return StrTabBuilder.getOffset(S); }
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

  // Adds symbols to the hash table.
  // Sorts the input to satisfy GNU hash section requirements.
  void addSymbols(std::vector<SymbolBody *> &Symbols);

private:
  static unsigned calcNBuckets(unsigned NumHashed);
  static unsigned calcMaskWords(unsigned NumHashed);

  void writeHeader(uint8_t *&Buf);
  void writeBloomFilter(uint8_t *&Buf);
  void writeHashTable(uint8_t *Buf);

  struct HashedSymbolData {
    SymbolBody *Body;
    uint32_t Hash;
  };

  std::vector<HashedSymbolData> HashedSymbols;

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

template <class ELFT>
class MipsReginfoOutputSection final : public OutputSectionBase<ELFT> {
  typedef llvm::object::Elf_Mips_RegInfo<ELFT> Elf_Mips_RegInfo;

public:
  MipsReginfoOutputSection();
  void writeTo(uint8_t *Buf) override;

  void addSection(MipsReginfoInputSection<ELFT> *S);

private:
  uint32_t GeneralMask = 0;
};

// All output sections that are hadnled by the linker specially are
// globally accessible. Writer initializes them, so don't use them
// until Writer is initialized.
template <class ELFT> struct Out {
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Phdr Elf_Phdr;
  static DynamicSection<ELFT> *Dynamic;
  static GnuHashTableSection<ELFT> *GnuHashTab;
  static GotPltSection<ELFT> *GotPlt;
  static GotSection<ELFT> *Got;
  static HashTableSection<ELFT> *HashTab;
  static InterpSection<ELFT> *Interp;
  static OutputSection<ELFT> *Bss;
  static OutputSection<ELFT> *MipsRldMap;
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
  static Elf_Phdr *TlsPhdr;
};

template <class ELFT> DynamicSection<ELFT> *Out<ELFT>::Dynamic;
template <class ELFT> GnuHashTableSection<ELFT> *Out<ELFT>::GnuHashTab;
template <class ELFT> GotPltSection<ELFT> *Out<ELFT>::GotPlt;
template <class ELFT> GotSection<ELFT> *Out<ELFT>::Got;
template <class ELFT> HashTableSection<ELFT> *Out<ELFT>::HashTab;
template <class ELFT> InterpSection<ELFT> *Out<ELFT>::Interp;
template <class ELFT> OutputSection<ELFT> *Out<ELFT>::Bss;
template <class ELFT> OutputSection<ELFT> *Out<ELFT>::MipsRldMap;
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
template <class ELFT> typename Out<ELFT>::Elf_Phdr *Out<ELFT>::TlsPhdr;

} // namespace elf2
} // namespace lld

#endif // LLD_ELF_OUTPUT_SECTIONS_H
