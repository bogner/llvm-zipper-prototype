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

#include "Config.h"
#include "Relocations.h"

#include "lld/Core/LLVM.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Object/ELF.h"

namespace lld {
namespace elf {

class SymbolBody;
struct EhSectionPiece;
template <class ELFT> class EhInputSection;
template <class ELFT> class InputSection;
template <class ELFT> class InputSectionBase;
template <class ELFT> class MergeInputSection;
template <class ELFT> class OutputSection;
template <class ELFT> class ObjectFile;
template <class ELFT> class SharedFile;
template <class ELFT> class SharedSymbol;
template <class ELFT> class DefinedRegular;

// This represents a section in an output file.
// Different sub classes represent different types of sections. Some contain
// input sections, others are created by the linker.
// The writer creates multiple OutputSections and assign them unique,
// non-overlapping file offsets and VAs.
class OutputSectionBase {
public:
  enum Kind {
    Base,
    EHFrame,
    Merge,
    Regular,
    VersDef,
    VersNeed,
    VersTable
  };

  OutputSectionBase(StringRef Name, uint32_t Type, uint64_t Flags);
  void setLMAOffset(uint64_t LMAOff) { LMAOffset = LMAOff; }
  uint64_t getLMA() const { return Addr + LMAOffset; }
  template <typename ELFT> void writeHeaderTo(typename ELFT::Shdr *SHdr);
  StringRef getName() const { return Name; }

  virtual void addSection(InputSectionData *C) {}
  virtual Kind getKind() const { return Base; }
  static bool classof(const OutputSectionBase *B) {
    return B->getKind() == Base;
  }

  unsigned SectionIndex;

  uint32_t getPhdrFlags() const;

  void updateAlignment(uint64_t Alignment) {
    if (Alignment > Addralign)
      Addralign = Alignment;
  }

  // If true, this section will be page aligned on disk.
  // Typically the first section of each PT_LOAD segment has this flag.
  bool PageAlign = false;

  // Pointer to the first section in PT_LOAD segment, which this section
  // also resides in. This field is used to correctly compute file offset
  // of a section. When two sections share the same load segment, difference
  // between their file offsets should be equal to difference between their
  // virtual addresses. To compute some section offset we use the following
  // formula: Off = Off_first + VA - VA_first.
  OutputSectionBase *FirstInPtLoad = nullptr;

  virtual void finalize() {}
  virtual void assignOffsets() {}
  virtual void writeTo(uint8_t *Buf) {}
  virtual ~OutputSectionBase() = default;

  StringRef Name;

  // The following fields correspond to Elf_Shdr members.
  uint64_t Size = 0;
  uint64_t Entsize = 0;
  uint64_t Addralign = 0;
  uint64_t Offset = 0;
  uint64_t Flags = 0;
  uint64_t LMAOffset = 0;
  uint64_t Addr = 0;
  uint32_t ShName = 0;
  uint32_t Type = 0;
  uint32_t Info = 0;
  uint32_t Link = 0;
};

// For more information about .gnu.version and .gnu.version_r see:
// https://www.akkadia.org/drepper/symbol-versioning

// The .gnu.version_d section which has a section type of SHT_GNU_verdef shall
// contain symbol version definitions. The number of entries in this section
// shall be contained in the DT_VERDEFNUM entry of the .dynamic section.
// The section shall contain an array of Elf_Verdef structures, optionally
// followed by an array of Elf_Verdaux structures.
template <class ELFT>
class VersionDefinitionSection final : public OutputSectionBase {
  typedef typename ELFT::Verdef Elf_Verdef;
  typedef typename ELFT::Verdaux Elf_Verdaux;

public:
  VersionDefinitionSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  Kind getKind() const override { return VersDef; }
  static bool classof(const OutputSectionBase *B) {
    return B->getKind() == VersDef;
  }

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
class VersionTableSection final : public OutputSectionBase {
  typedef typename ELFT::Versym Elf_Versym;

public:
  VersionTableSection();
  void finalize() override;
  void writeTo(uint8_t *Buf) override;
  Kind getKind() const override { return VersTable; }
  static bool classof(const OutputSectionBase *B) {
    return B->getKind() == VersTable;
  }
};

// The .gnu.version_r section defines the version identifiers used by
// .gnu.version. It contains a linked list of Elf_Verneed data structures. Each
// Elf_Verneed specifies the version requirements for a single DSO, and contains
// a reference to a linked list of Elf_Vernaux data structures which define the
// mapping from version identifiers to version names.
template <class ELFT>
class VersionNeedSection final : public OutputSectionBase {
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
  size_t getNeedNum() const { return Needed.size(); }
  Kind getKind() const override { return VersNeed; }
  static bool classof(const OutputSectionBase *B) {
    return B->getKind() == VersNeed;
  }
};

template <class ELFT> class OutputSection final : public OutputSectionBase {

public:
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::Rel Elf_Rel;
  typedef typename ELFT::Rela Elf_Rela;
  typedef typename ELFT::uint uintX_t;
  OutputSection(StringRef Name, uint32_t Type, uintX_t Flags);
  void addSection(InputSectionData *C) override;
  void sort(std::function<unsigned(InputSection<ELFT> *S)> Order);
  void sortInitFini();
  void sortCtorsDtors();
  void writeTo(uint8_t *Buf) override;
  void finalize() override;
  void assignOffsets() override;
  Kind getKind() const override { return Regular; }
  static bool classof(const OutputSectionBase *B) {
    return B->getKind() == Regular;
  }
  std::vector<InputSection<ELFT> *> Sections;
};

template <class ELFT>
class MergeOutputSection final : public OutputSectionBase {
  typedef typename ELFT::uint uintX_t;

public:
  MergeOutputSection(StringRef Name, uint32_t Type, uintX_t Flags,
                     uintX_t Alignment);
  void addSection(InputSectionData *S) override;
  void writeTo(uint8_t *Buf) override;
  void finalize() override;
  bool shouldTailMerge() const;
  Kind getKind() const override { return Merge; }
  static bool classof(const OutputSectionBase *B) {
    return B->getKind() == Merge;
  }

private:
  llvm::StringTableBuilder Builder;
  std::vector<MergeInputSection<ELFT> *> Sections;
};

struct CieRecord {
  EhSectionPiece *Piece = nullptr;
  std::vector<EhSectionPiece *> FdePieces;
};

// Output section for .eh_frame.
template <class ELFT> class EhOutputSection final : public OutputSectionBase {
  typedef typename ELFT::uint uintX_t;
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::Rel Elf_Rel;
  typedef typename ELFT::Rela Elf_Rela;

public:
  EhOutputSection();
  void writeTo(uint8_t *Buf) override;
  void finalize() override;
  bool empty() const { return Sections.empty(); }

  void addSection(InputSectionData *S) override;
  Kind getKind() const override { return EHFrame; }
  static bool classof(const OutputSectionBase *B) {
    return B->getKind() == EHFrame;
  }

  size_t NumFdes = 0;

private:
  template <class RelTy>
  void addSectionAux(EhInputSection<ELFT> *S, llvm::ArrayRef<RelTy> Rels);

  template <class RelTy>
  CieRecord *addCie(EhSectionPiece &Piece, EhInputSection<ELFT> *Sec,
                    ArrayRef<RelTy> Rels);

  template <class RelTy>
  bool isFdeLive(EhSectionPiece &Piece, EhInputSection<ELFT> *Sec,
                 ArrayRef<RelTy> Rels);

  uintX_t getFdePc(uint8_t *Buf, size_t Off, uint8_t Enc);

  std::vector<EhInputSection<ELFT> *> Sections;
  std::vector<CieRecord *> Cies;

  // CIE records are uniquified by their contents and personality functions.
  llvm::DenseMap<std::pair<ArrayRef<uint8_t>, SymbolBody *>, CieRecord> CieMap;
};

// All output sections that are hadnled by the linker specially are
// globally accessible. Writer initializes them, so don't use them
// until Writer is initialized.
template <class ELFT> struct Out {
  typedef typename ELFT::uint uintX_t;
  typedef typename ELFT::Phdr Elf_Phdr;

  static uint8_t First;
  static EhOutputSection<ELFT> *EhFrame;
  static OutputSection<ELFT> *Bss;
  static OutputSection<ELFT> *MipsRldMap;
  static OutputSectionBase *Opd;
  static uint8_t *OpdBuf;
  static VersionDefinitionSection<ELFT> *VerDef;
  static VersionTableSection<ELFT> *VerSym;
  static VersionNeedSection<ELFT> *VerNeed;
  static Elf_Phdr *TlsPhdr;
  static OutputSectionBase *DebugInfo;
  static OutputSectionBase *ElfHeader;
  static OutputSectionBase *ProgramHeaders;
  static OutputSectionBase *PreinitArray;
  static OutputSectionBase *InitArray;
  static OutputSectionBase *FiniArray;
};

template <bool Is64Bits> struct SectionKey {
  typedef typename std::conditional<Is64Bits, uint64_t, uint32_t>::type uintX_t;
  StringRef Name;
  uint32_t Type;
  uintX_t Flags;
  uintX_t Alignment;
};

// This class knows how to create an output section for a given
// input section. Output section type is determined by various
// factors, including input section's sh_flags, sh_type and
// linker scripts.
template <class ELFT> class OutputSectionFactory {
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::uint uintX_t;
  typedef typename elf::SectionKey<ELFT::Is64Bits> Key;

public:
  std::pair<OutputSectionBase *, bool> create(InputSectionBase<ELFT> *C,
                                              StringRef OutsecName);
  std::pair<OutputSectionBase *, bool>
  create(const SectionKey<ELFT::Is64Bits> &Key, InputSectionBase<ELFT> *C);

private:
  llvm::SmallDenseMap<Key, OutputSectionBase *> Map;
};

template <class ELFT> uint64_t getHeaderSize() {
  if (Config->OFormatBinary)
    return 0;
  return Out<ELFT>::ElfHeader->Size + Out<ELFT>::ProgramHeaders->Size;
}

template <class ELFT> uint8_t Out<ELFT>::First;
template <class ELFT> EhOutputSection<ELFT> *Out<ELFT>::EhFrame;
template <class ELFT> OutputSection<ELFT> *Out<ELFT>::Bss;
template <class ELFT> OutputSection<ELFT> *Out<ELFT>::MipsRldMap;
template <class ELFT> OutputSectionBase *Out<ELFT>::Opd;
template <class ELFT> uint8_t *Out<ELFT>::OpdBuf;
template <class ELFT> VersionDefinitionSection<ELFT> *Out<ELFT>::VerDef;
template <class ELFT> VersionTableSection<ELFT> *Out<ELFT>::VerSym;
template <class ELFT> VersionNeedSection<ELFT> *Out<ELFT>::VerNeed;
template <class ELFT> typename ELFT::Phdr *Out<ELFT>::TlsPhdr;
template <class ELFT> OutputSectionBase *Out<ELFT>::DebugInfo;
template <class ELFT> OutputSectionBase *Out<ELFT>::ElfHeader;
template <class ELFT> OutputSectionBase *Out<ELFT>::ProgramHeaders;
template <class ELFT> OutputSectionBase *Out<ELFT>::PreinitArray;
template <class ELFT> OutputSectionBase *Out<ELFT>::InitArray;
template <class ELFT> OutputSectionBase *Out<ELFT>::FiniArray;
} // namespace elf
} // namespace lld

namespace llvm {
template <bool Is64Bits> struct DenseMapInfo<lld::elf::SectionKey<Is64Bits>> {
  typedef typename lld::elf::SectionKey<Is64Bits> Key;

  static Key getEmptyKey();
  static Key getTombstoneKey();
  static unsigned getHashValue(const Key &Val);
  static bool isEqual(const Key &LHS, const Key &RHS);
};
}

#endif
