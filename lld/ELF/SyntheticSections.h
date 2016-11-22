//===- SyntheticSection.h ---------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_SYNTHETIC_SECTION_H
#define LLD_ELF_SYNTHETIC_SECTION_H

#include "GdbIndex.h"
#include "InputSection.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace lld {
namespace elf {

// .MIPS.abiflags section.
template <class ELFT>
class MipsAbiFlagsSection final : public InputSection<ELFT> {
  typedef llvm::object::Elf_Mips_ABIFlags<ELFT> Elf_Mips_ABIFlags;

public:
  MipsAbiFlagsSection();

private:
  Elf_Mips_ABIFlags Flags = {};
};

// .MIPS.options section.
template <class ELFT>
class MipsOptionsSection final : public InputSection<ELFT> {
  typedef llvm::object::Elf_Mips_Options<ELFT> Elf_Mips_Options;
  typedef llvm::object::Elf_Mips_RegInfo<ELFT> Elf_Mips_RegInfo;

public:
  MipsOptionsSection();
  void finalize();

private:
  std::vector<uint8_t> Buf;

  Elf_Mips_Options *getOptions() {
    return reinterpret_cast<Elf_Mips_Options *>(Buf.data());
  }
};

// MIPS .reginfo section.
template <class ELFT>
class MipsReginfoSection final : public InputSection<ELFT> {
  typedef llvm::object::Elf_Mips_RegInfo<ELFT> Elf_Mips_RegInfo;

public:
  MipsReginfoSection();
  void finalize();

private:
  Elf_Mips_RegInfo Reginfo = {};
};

template <class ELFT> class SyntheticSection : public InputSection<ELFT> {
  typedef typename ELFT::uint uintX_t;

public:
  SyntheticSection(uintX_t Flags, uint32_t Type, uintX_t Addralign,
                   StringRef Name)
      : InputSection<ELFT>(Flags, Type, Addralign, ArrayRef<uint8_t>(), Name,
                           InputSectionData::Synthetic) {
    this->Live = true;
  }

  virtual void writeTo(uint8_t *Buf) = 0;
  virtual size_t getSize() const { return this->Data.size(); }
  virtual void finalize() {}
  uintX_t getVA() const {
    return this->OutSec ? this->OutSec->Addr + this->OutSecOff : 0;
  }

  static bool classof(const InputSectionData *D) {
    return D->kind() == InputSectionData::Synthetic;
  }

protected:
  ~SyntheticSection() = default;
};

// .note.gnu.build-id section.
template <class ELFT> class BuildIdSection : public InputSection<ELFT> {
public:
  BuildIdSection();
  void writeBuildId(llvm::MutableArrayRef<uint8_t> Buf);

private:
  // First 16 bytes are a header.
  static const unsigned HeaderSize = 16;

  size_t getHashSize();
  uint8_t *getOutputLoc(uint8_t *Start);

  void
  computeHash(llvm::MutableArrayRef<uint8_t> Buf,
              std::function<void(ArrayRef<uint8_t> Arr, uint8_t *Hash)> Hash);

  std::vector<uint8_t> Buf;
  size_t HashSize;
};

template <class ELFT> class GotSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::uint uintX_t;

public:
  GotSection();
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return Size; }
  void finalize() override;
  void addEntry(SymbolBody &Sym);
  bool addDynTlsEntry(SymbolBody &Sym);
  bool addTlsIndex();
  bool empty() const { return Entries.empty(); }
  uintX_t getGlobalDynAddr(const SymbolBody &B) const;
  uintX_t getGlobalDynOffset(const SymbolBody &B) const;

  uintX_t getTlsIndexVA() { return this->getVA() + TlsIndexOff; }
  uint32_t getTlsIndexOff() const { return TlsIndexOff; }

  // Flag to force GOT to be in output if we have relocations
  // that relies on its address.
  bool HasGotOffRel = false;

private:
  std::vector<const SymbolBody *> Entries;
  uint32_t TlsIndexOff = -1;
  uintX_t Size = 0;
};

template <class ELFT> class MipsGotSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::uint uintX_t;

public:
  MipsGotSection();
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return Size; }
  void finalize() override;
  void addEntry(SymbolBody &Sym, uintX_t Addend, RelExpr Expr);
  bool addDynTlsEntry(SymbolBody &Sym);
  bool addTlsIndex();
  bool empty() const { return PageEntriesNum == 0 && TlsEntries.empty(); }
  uintX_t getPageEntryOffset(uintX_t Addr);
  uintX_t getBodyEntryOffset(const SymbolBody &B, uintX_t Addend) const;
  uintX_t getGlobalDynOffset(const SymbolBody &B) const;

  // Returns the symbol which corresponds to the first entry of the global part
  // of GOT on MIPS platform. It is required to fill up MIPS-specific dynamic
  // table properties.
  // Returns nullptr if the global part is empty.
  const SymbolBody *getFirstGlobalEntry() const;

  // Returns the number of entries in the local part of GOT including
  // the number of reserved entries.
  unsigned getLocalEntriesNum() const;

  // Returns offset of TLS part of the MIPS GOT table. This part goes
  // after 'local' and 'global' entries.
  uintX_t getTlsOffset() const;

  uint32_t getTlsIndexOff() const { return TlsIndexOff; }

private:
  // MIPS GOT consists of three parts: local, global and tls. Each part
  // contains different types of entries. Here is a layout of GOT:
  // - Header entries                |
  // - Page entries                  |   Local part
  // - Local entries (16-bit access) |
  // - Local entries (32-bit access) |
  // - Normal global entries         ||  Global part
  // - Reloc-only global entries     ||
  // - TLS entries                   ||| TLS part
  //
  // Header:
  //   Two entries hold predefined value 0x0 and 0x80000000.
  // Page entries:
  //   These entries created by R_MIPS_GOT_PAGE relocation and R_MIPS_GOT16
  //   relocation against local symbols. They are initialized by higher 16-bit
  //   of the corresponding symbol's value. So each 64kb of address space
  //   requires a single GOT entry.
  // Local entries (16-bit access):
  //   These entries created by GOT relocations against global non-preemptible
  //   symbols so dynamic linker is not necessary to resolve the symbol's
  //   values. "16-bit access" means that corresponding relocations address
  //   GOT using 16-bit index. Each unique Symbol-Addend pair has its own
  //   GOT entry.
  // Local entries (32-bit access):
  //   These entries are the same as above but created by relocations which
  //   address GOT using 32-bit index (R_MIPS_GOT_HI16/LO16 etc).
  // Normal global entries:
  //   These entries created by GOT relocations against preemptible global
  //   symbols. They need to be initialized by dynamic linker and they ordered
  //   exactly as the corresponding entries in the dynamic symbols table.
  // Reloc-only global entries:
  //   These entries created for symbols that are referenced by dynamic
  //   relocations R_MIPS_REL32. These entries are not accessed with gp-relative
  //   addressing, but MIPS ABI requires that these entries be present in GOT.
  // TLS entries:
  //   Entries created by TLS relocations.

  // Total number of allocated "Header" and "Page" entries.
  uint32_t PageEntriesNum = 0;
  // Output sections referenced by MIPS GOT relocations.
  llvm::SmallPtrSet<const OutputSectionBase *, 10> OutSections;
  // Map from "page" address to the GOT index.
  llvm::DenseMap<uintX_t, size_t> PageIndexMap;

  typedef std::pair<const SymbolBody *, uintX_t> GotEntry;
  typedef std::vector<GotEntry> GotEntries;
  // Map from Symbol-Addend pair to the GOT index.
  llvm::DenseMap<GotEntry, size_t> EntryIndexMap;
  // Local entries (16-bit access).
  GotEntries LocalEntries;
  // Local entries (32-bit access).
  GotEntries LocalEntries32;

  // Normal and reloc-only global entries.
  GotEntries GlobalEntries;

  // TLS entries.
  std::vector<const SymbolBody *> TlsEntries;

  uint32_t TlsIndexOff = -1;
  uintX_t Size = 0;
};

template <class ELFT>
class GotPltSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::uint uintX_t;

public:
  GotPltSection();
  void addEntry(SymbolBody &Sym);
  bool empty() const;
  size_t getSize() const override;
  void writeTo(uint8_t *Buf) override;

private:
  std::vector<const SymbolBody *> Entries;
};

template <class ELFT>
class StringTableSection final : public SyntheticSection<ELFT> {
public:
  typedef typename ELFT::uint uintX_t;
  StringTableSection(StringRef Name, bool Dynamic);
  unsigned addString(StringRef S, bool HashIt = true);
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return Size; }
  bool isDynamic() const { return Dynamic; }

private:
  const bool Dynamic;

  // ELF string tables start with a NUL byte, so 1.
  uintX_t Size = 1;

  llvm::DenseMap<StringRef, unsigned> StringMap;
  std::vector<StringRef> Strings;
};

template <class ELFT> class DynamicReloc {
  typedef typename ELFT::uint uintX_t;

public:
  DynamicReloc(uint32_t Type, const InputSectionBase<ELFT> *InputSec,
               uintX_t OffsetInSec, bool UseSymVA, SymbolBody *Sym,
               uintX_t Addend)
      : Type(Type), Sym(Sym), InputSec(InputSec), OffsetInSec(OffsetInSec),
        UseSymVA(UseSymVA), Addend(Addend) {}

  DynamicReloc(uint32_t Type, const OutputSectionBase *OutputSec,
               uintX_t OffsetInSec, bool UseSymVA, SymbolBody *Sym,
               uintX_t Addend)
      : Type(Type), Sym(Sym), OutputSec(OutputSec), OffsetInSec(OffsetInSec),
        UseSymVA(UseSymVA), Addend(Addend) {}

  uintX_t getOffset() const;
  uintX_t getAddend() const;
  uint32_t getSymIndex() const;
  const OutputSectionBase *getOutputSec() const { return OutputSec; }
  const InputSectionBase<ELFT> *getInputSec() const { return InputSec; }

  uint32_t Type;

private:
  SymbolBody *Sym;
  const InputSectionBase<ELFT> *InputSec = nullptr;
  const OutputSectionBase *OutputSec = nullptr;
  uintX_t OffsetInSec;
  bool UseSymVA;
  uintX_t Addend;
};

template <class ELFT>
class DynamicSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::Dyn Elf_Dyn;
  typedef typename ELFT::Rel Elf_Rel;
  typedef typename ELFT::Rela Elf_Rela;
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::uint uintX_t;

  // The .dynamic section contains information for the dynamic linker.
  // The section consists of fixed size entries, which consist of
  // type and value fields. Value are one of plain integers, symbol
  // addresses, or section addresses. This struct represents the entry.
  struct Entry {
    int32_t Tag;
    union {
      OutputSectionBase *OutSec;
      InputSection<ELFT> *InSec;
      uint64_t Val;
      const SymbolBody *Sym;
    };
    enum KindT { SecAddr, SecSize, SymAddr, PlainInt, InSecAddr } Kind;
    Entry(int32_t Tag, OutputSectionBase *OutSec, KindT Kind = SecAddr)
        : Tag(Tag), OutSec(OutSec), Kind(Kind) {}
    Entry(int32_t Tag, InputSection<ELFT> *Sec)
        : Tag(Tag), InSec(Sec), Kind(InSecAddr) {}
    Entry(int32_t Tag, uint64_t Val) : Tag(Tag), Val(Val), Kind(PlainInt) {}
    Entry(int32_t Tag, const SymbolBody *Sym)
        : Tag(Tag), Sym(Sym), Kind(SymAddr) {}
  };

  // finalize() fills this vector with the section contents. finalize()
  // cannot directly create final section contents because when the
  // function is called, symbol or section addresses are not fixed yet.
  std::vector<Entry> Entries;

public:
  DynamicSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return Size; }

private:
  void addEntries();
  void add(Entry E) { Entries.push_back(E); }
  uintX_t Size = 0;
};

template <class ELFT>
class RelocationSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::Rel Elf_Rel;
  typedef typename ELFT::Rela Elf_Rela;
  typedef typename ELFT::uint uintX_t;

public:
  RelocationSection(StringRef Name, bool Sort);
  void addReloc(const DynamicReloc<ELFT> &Reloc);
  unsigned getRelocOffset();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return Relocs.size() * this->Entsize; }
  bool hasRelocs() const { return !Relocs.empty(); }
  size_t getRelativeRelocCount() const { return NumRelativeRelocs; }

private:
  bool Sort;
  size_t NumRelativeRelocs = 0;
  std::vector<DynamicReloc<ELFT>> Relocs;
};

struct SymbolTableEntry {
  SymbolBody *Symbol;
  size_t StrTabOffset;
};

template <class ELFT>
class SymbolTableSection final : public SyntheticSection<ELFT> {
public:
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::SymRange Elf_Sym_Range;
  typedef typename ELFT::uint uintX_t;
  SymbolTableSection(StringTableSection<ELFT> &StrTabSec);

  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return getNumSymbols() * sizeof(Elf_Sym); }
  void addSymbol(SymbolBody *Body);
  StringTableSection<ELFT> &getStrTabSec() const { return StrTabSec; }
  unsigned getNumSymbols() const { return NumLocals + Symbols.size() + 1; }

  ArrayRef<SymbolTableEntry> getSymbols() const { return Symbols; }

  unsigned NumLocals = 0;
  StringTableSection<ELFT> &StrTabSec;

private:
  void writeLocalSymbols(uint8_t *&Buf);
  void writeGlobalSymbols(uint8_t *Buf);

  const OutputSectionBase *getOutputSection(SymbolBody *Sym);

  // A vector of symbols and their string table offsets.
  std::vector<SymbolTableEntry> Symbols;
};

// Outputs GNU Hash section. For detailed explanation see:
// https://blogs.oracle.com/ali/entry/gnu_hash_elf_sections
template <class ELFT>
class GnuHashTableSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::Off Elf_Off;
  typedef typename ELFT::Word Elf_Word;
  typedef typename ELFT::uint uintX_t;

public:
  GnuHashTableSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return this->Size; }

  // Adds symbols to the hash table.
  // Sorts the input to satisfy GNU hash section requirements.
  void addSymbols(std::vector<SymbolTableEntry> &Symbols);

private:
  static unsigned calcNBuckets(unsigned NumHashed);
  static unsigned calcMaskWords(unsigned NumHashed);

  void writeHeader(uint8_t *&Buf);
  void writeBloomFilter(uint8_t *&Buf);
  void writeHashTable(uint8_t *Buf);

  struct SymbolData {
    SymbolBody *Body;
    size_t STName;
    uint32_t Hash;
  };

  std::vector<SymbolData> Symbols;

  unsigned MaskWords;
  unsigned NBuckets;
  unsigned Shift2;
  uintX_t Size = 0;
};

template <class ELFT>
class HashTableSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::Word Elf_Word;

public:
  HashTableSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return this->Size; }

private:
  size_t Size = 0;
};

template <class ELFT> class PltSection final : public SyntheticSection<ELFT> {
public:
  PltSection();
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override;
  void addEntry(SymbolBody &Sym);
  bool empty() const { return Entries.empty(); }

private:
  std::vector<std::pair<const SymbolBody *, unsigned>> Entries;
};

template <class ELFT>
class GdbIndexSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::uint uintX_t;

  const unsigned OffsetTypeSize = 4;
  const unsigned CuListOffset = 6 * OffsetTypeSize;
  const unsigned CompilationUnitSize = 16;
  const unsigned AddressEntrySize = 16 + OffsetTypeSize;
  const unsigned SymTabEntrySize = 2 * OffsetTypeSize;

public:
  GdbIndexSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return CuTypesOffset; }

  // Pairs of [CU Offset, CU length].
  std::vector<std::pair<uintX_t, uintX_t>> CompilationUnits;

private:
  void parseDebugSections();
  void readDwarf(InputSection<ELFT> *I);

  uint32_t CuTypesOffset;
};

// --eh-frame-hdr option tells linker to construct a header for all the
// .eh_frame sections. This header is placed to a section named .eh_frame_hdr
// and also to a PT_GNU_EH_FRAME segment.
// At runtime the unwinder then can find all the PT_GNU_EH_FRAME segments by
// calling dl_iterate_phdr.
// This section contains a lookup table for quick binary search of FDEs.
// Detailed info about internals can be found in Ian Lance Taylor's blog:
// http://www.airs.com/blog/archives/460 (".eh_frame")
// http://www.airs.com/blog/archives/462 (".eh_frame_hdr")
template <class ELFT>
class EhFrameHeader final : public SyntheticSection<ELFT> {
  typedef typename ELFT::uint uintX_t;

public:
  EhFrameHeader();
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override;
  void addFde(uint32_t Pc, uint32_t FdeVA);

private:
  struct FdeData {
    uint32_t Pc;
    uint32_t FdeVA;
  };

  std::vector<FdeData> Fdes;
};

// For more information about .gnu.version and .gnu.version_r see:
// https://www.akkadia.org/drepper/symbol-versioning

// The .gnu.version_d section which has a section type of SHT_GNU_verdef shall
// contain symbol version definitions. The number of entries in this section
// shall be contained in the DT_VERDEFNUM entry of the .dynamic section.
// The section shall contain an array of Elf_Verdef structures, optionally
// followed by an array of Elf_Verdaux structures.
template <class ELFT>
class VersionDefinitionSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::Verdef Elf_Verdef;
  typedef typename ELFT::Verdaux Elf_Verdaux;

public:
  VersionDefinitionSection();
  void finalize() override;
  size_t getSize() const override;
  void writeTo(uint8_t *Buf) override;

private:
  void writeOne(uint8_t *Buf, uint32_t Index, StringRef Name, size_t NameOff);

  unsigned FileDefNameOff;
};

// The .gnu.version section specifies the required version of each symbol in the
// dynamic symbol table. It contains one Elf_Versym for each dynamic symbol
// table entry. An Elf_Versym is just a 16-bit integer that refers to a version
// identifier defined in the either .gnu.version_r or .gnu.version_d section.
// The values 0 and 1 are reserved. All other values are used for versions in
// the own object or in any of the dependencies.
template <class ELFT>
class VersionTableSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::Versym Elf_Versym;

public:
  VersionTableSection();
  void finalize() override;
  size_t getSize() const override;
  void writeTo(uint8_t *Buf) override;
};

// The .gnu.version_r section defines the version identifiers used by
// .gnu.version. It contains a linked list of Elf_Verneed data structures. Each
// Elf_Verneed specifies the version requirements for a single DSO, and contains
// a reference to a linked list of Elf_Vernaux data structures which define the
// mapping from version identifiers to version names.
template <class ELFT>
class VersionNeedSection final : public SyntheticSection<ELFT> {
  typedef typename ELFT::Verneed Elf_Verneed;
  typedef typename ELFT::Vernaux Elf_Vernaux;

  // A vector of shared files that need Elf_Verneed data structures and the
  // string table offsets of their sonames.
  std::vector<std::pair<SharedFile<ELFT> *, size_t>> Needed;

  // The next available version identifier.
  unsigned NextIndex;

public:
  VersionNeedSection();
  void addSymbol(SharedSymbol<ELFT> *SS);
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override;
  size_t getNeedNum() const { return Needed.size(); }
};

template <class ELFT> InputSection<ELFT> *createCommonSection();
template <class ELFT> InputSection<ELFT> *createInterpSection();
template <class ELFT> MergeInputSection<ELFT> *createCommentSection();

// Linker generated sections which can be used as inputs.
template <class ELFT> struct In {
  static BuildIdSection<ELFT> *BuildId;
  static InputSection<ELFT> *Common;
  static DynamicSection<ELFT> *Dynamic;
  static StringTableSection<ELFT> *DynStrTab;
  static SymbolTableSection<ELFT> *DynSymTab;
  static EhFrameHeader<ELFT> *EhFrameHdr;
  static GnuHashTableSection<ELFT> *GnuHashTab;
  static GdbIndexSection<ELFT> *GdbIndex;
  static GotSection<ELFT> *Got;
  static MipsGotSection<ELFT> *MipsGot;
  static GotPltSection<ELFT> *GotPlt;
  static HashTableSection<ELFT> *HashTab;
  static InputSection<ELFT> *Interp;
  static MipsAbiFlagsSection<ELFT> *MipsAbiFlags;
  static MipsOptionsSection<ELFT> *MipsOptions;
  static MipsReginfoSection<ELFT> *MipsReginfo;
  static PltSection<ELFT> *Plt;
  static RelocationSection<ELFT> *RelaDyn;
  static RelocationSection<ELFT> *RelaPlt;
  static StringTableSection<ELFT> *ShStrTab;
  static StringTableSection<ELFT> *StrTab;
  static SymbolTableSection<ELFT> *SymTab;
  static VersionDefinitionSection<ELFT> *VerDef;
  static VersionTableSection<ELFT> *VerSym;
  static VersionNeedSection<ELFT> *VerNeed;
};

template <class ELFT> BuildIdSection<ELFT> *In<ELFT>::BuildId;
template <class ELFT> InputSection<ELFT> *In<ELFT>::Common;
template <class ELFT> DynamicSection<ELFT> *In<ELFT>::Dynamic;
template <class ELFT> StringTableSection<ELFT> *In<ELFT>::DynStrTab;
template <class ELFT> SymbolTableSection<ELFT> *In<ELFT>::DynSymTab;
template <class ELFT> EhFrameHeader<ELFT> *In<ELFT>::EhFrameHdr;
template <class ELFT> GdbIndexSection<ELFT> *In<ELFT>::GdbIndex;
template <class ELFT> GnuHashTableSection<ELFT> *In<ELFT>::GnuHashTab;
template <class ELFT> GotSection<ELFT> *In<ELFT>::Got;
template <class ELFT> MipsGotSection<ELFT> *In<ELFT>::MipsGot;
template <class ELFT> GotPltSection<ELFT> *In<ELFT>::GotPlt;
template <class ELFT> HashTableSection<ELFT> *In<ELFT>::HashTab;
template <class ELFT> InputSection<ELFT> *In<ELFT>::Interp;
template <class ELFT> MipsAbiFlagsSection<ELFT> *In<ELFT>::MipsAbiFlags;
template <class ELFT> MipsOptionsSection<ELFT> *In<ELFT>::MipsOptions;
template <class ELFT> MipsReginfoSection<ELFT> *In<ELFT>::MipsReginfo;
template <class ELFT> PltSection<ELFT> *In<ELFT>::Plt;
template <class ELFT> RelocationSection<ELFT> *In<ELFT>::RelaDyn;
template <class ELFT> RelocationSection<ELFT> *In<ELFT>::RelaPlt;
template <class ELFT> StringTableSection<ELFT> *In<ELFT>::ShStrTab;
template <class ELFT> StringTableSection<ELFT> *In<ELFT>::StrTab;
template <class ELFT> SymbolTableSection<ELFT> *In<ELFT>::SymTab;
template <class ELFT> VersionDefinitionSection<ELFT> *In<ELFT>::VerDef;
template <class ELFT> VersionTableSection<ELFT> *In<ELFT>::VerSym;
template <class ELFT> VersionNeedSection<ELFT> *In<ELFT>::VerNeed;
} // namespace elf
} // namespace lld

#endif
