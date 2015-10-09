//===- Writer.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "Config.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Target.h"

#include "llvm/Support/FileOutputBuffer.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;

using namespace lld;
using namespace lld::elf2;

namespace {

static uint32_t toPHDRFlags(uint64_t Flags) {
  uint32_t Ret = PF_R;
  if (Flags & SHF_WRITE)
    Ret |= PF_W;
  if (Flags & SHF_EXECINSTR)
    Ret |= PF_X;
  return Ret;
}

template <class ELFT> struct ProgramHeader {
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename ELFFile<ELFT>::Elf_Phdr Elf_Phdr;

  ProgramHeader(uintX_t Type, uintX_t Flags, uintX_t FileOff, uintX_t VA) {
    std::memset(&Header, 0, sizeof(Elf_Phdr));
    Header.p_type = Type;
    Header.p_flags = Flags;
    Header.p_align = Target->getPageSize();
    Header.p_offset = FileOff;
    Header.p_vaddr = VA;
    Header.p_paddr = VA;
  }

  void setValuesFromSection(OutputSectionBase<ELFT::Is64Bits> *Sec) {
    Header.p_flags = toPHDRFlags(Sec->getFlags());
    Header.p_offset = Sec->getFileOff();
    Header.p_vaddr = Sec->getVA();
    Header.p_paddr = Header.p_vaddr;
    Header.p_filesz = Sec->getSize();
    Header.p_memsz = Header.p_filesz;
    Header.p_align = Sec->getAlign();
  }

  Elf_Phdr Header;
  bool Closed = false;
};

// The writer writes a SymbolTable result to a file.
template <class ELFT> class Writer {
public:
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  typedef typename ELFFile<ELFT>::Elf_Ehdr Elf_Ehdr;
  typedef typename ELFFile<ELFT>::Elf_Phdr Elf_Phdr;
  typedef typename ELFFile<ELFT>::Elf_Sym Elf_Sym;
  typedef typename ELFFile<ELFT>::Elf_Sym_Range Elf_Sym_Range;
  typedef typename ELFFile<ELFT>::Elf_Rela Elf_Rela;
  Writer(SymbolTable<ELFT> &S) : Symtab(S) {}
  void run();

private:
  void copyLocalSymbols();
  void createSections();
  template <bool isRela>
  void scanRelocs(const InputSection<ELFT> &C,
                  iterator_range<const Elf_Rel_Impl<ELFT, isRela> *> Rels);
  void scanRelocs(const InputSection<ELFT> &C);
  void assignAddresses();
  void openFile(StringRef OutputPath);
  void writeHeader();
  void writeSections();
  bool needsInterpSection() const {
    return !Symtab.getSharedFiles().empty() && !Config->DynamicLinker.empty();
  }
  bool isOutputDynamic() const {
    return !Symtab.getSharedFiles().empty() || Config->Shared;
  }
  bool needsDynamicSections() const { return isOutputDynamic(); }
  uintX_t getVAStart() const { return Config->Shared ? 0 : Target->getVAStart(); }

  std::unique_ptr<llvm::FileOutputBuffer> Buffer;

  llvm::SpecificBumpPtrAllocator<OutputSection<ELFT>> CAlloc;
  std::vector<OutputSectionBase<ELFT::Is64Bits> *> OutputSections;
  unsigned getNumSections() const { return OutputSections.size() + 1; }

  llvm::BumpPtrAllocator PAlloc;
  SymbolTable<ELFT> &Symtab;
  std::vector<ProgramHeader<ELFT> *> PHDRs;
  ProgramHeader<ELFT> PhdrPHDR{PT_PHDR, PF_R, 0, 0};
  ProgramHeader<ELFT> FileHeaderPHDR{PT_LOAD, PF_R, 0, 0};
  ProgramHeader<ELFT> InterpPHDR{PT_INTERP, 0, 0, 0};
  ProgramHeader<ELFT> DynamicPHDR{PT_DYNAMIC, 0, 0, 0};

  uintX_t FileSize;
  uintX_t ProgramHeaderOff;
  uintX_t SectionHeaderOff;
};
} // anonymous namespace

template <class ELFT> void lld::elf2::writeResult(SymbolTable<ELFT> *Symtab) {
  // Initialize output sections that are handled by Writer specially.
  // Don't reorder because the order of initialization matters.
  InterpSection<ELFT::Is64Bits> Interp;
  Out<ELFT>::Interp = &Interp;
  StringTableSection<ELFT::Is64Bits> StrTab(false);
  Out<ELFT>::StrTab = &StrTab;
  StringTableSection<ELFT::Is64Bits> DynStrTab(true);
  Out<ELFT>::DynStrTab = &DynStrTab;
  OutputSection<ELFT> Bss(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
  Out<ELFT>::Bss = &Bss;
  GotSection<ELFT> Got;
  Out<ELFT>::Got = &Got;
  PltSection<ELFT> Plt;
  Out<ELFT>::Plt = &Plt;
  SymbolTableSection<ELFT> SymTab(*Symtab, *Out<ELFT>::StrTab);
  Out<ELFT>::SymTab = &SymTab;
  SymbolTableSection<ELFT> DynSymTab(*Symtab, *Out<ELFT>::DynStrTab);
  Out<ELFT>::DynSymTab = &DynSymTab;
  HashTableSection<ELFT> HashTab;
  Out<ELFT>::HashTab = &HashTab;
  RelocationSection<ELFT> RelaDyn(Symtab->shouldUseRela());
  Out<ELFT>::RelaDyn = &RelaDyn;
  DynamicSection<ELFT> Dynamic(*Symtab);
  Out<ELFT>::Dynamic = &Dynamic;

  Writer<ELFT>(*Symtab).run();
}

// The main function of the writer.
template <class ELFT> void Writer<ELFT>::run() {
  if (!Config->DiscardAll)
    copyLocalSymbols();
  createSections();
  assignAddresses();
  openFile(Config->OutputFile);
  writeHeader();
  writeSections();
  error(Buffer->commit());
}

namespace {
template <bool Is64Bits> struct SectionKey {
  typedef typename std::conditional<Is64Bits, uint64_t, uint32_t>::type uintX_t;
  StringRef Name;
  uint32_t Type;
  uintX_t Flags;
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
    return hash_combine(Val.Name, Val.Type, Val.Flags);
  }
  static bool isEqual(const SectionKey<Is64Bits> &LHS,
                      const SectionKey<Is64Bits> &RHS) {
    return DenseMapInfo<StringRef>::isEqual(LHS.Name, RHS.Name) &&
           LHS.Type == RHS.Type && LHS.Flags == RHS.Flags;
  }
};
}

// The reason we have to do this early scan is as follows
// * To mmap the output file, we need to know the size
// * For that, we need to know how many dynamic relocs we will have.
// It might be possible to avoid this by outputting the file with write:
// * Write the allocated output sections, computing addresses.
// * Apply relocations, recording which ones require a dynamic reloc.
// * Write the dynamic relocations.
// * Write the rest of the file.
template <class ELFT>
template <bool isRela>
void Writer<ELFT>::scanRelocs(
    const InputSection<ELFT> &C,
    iterator_range<const Elf_Rel_Impl<ELFT, isRela> *> Rels) {
  typedef Elf_Rel_Impl<ELFT, isRela> RelType;
  const ObjectFile<ELFT> &File = *C.getFile();
  bool IsMips64EL = File.getObj().isMips64EL();
  for (const RelType &RI : Rels) {
    uint32_t SymIndex = RI.getSymbol(IsMips64EL);
    SymbolBody *Body = File.getSymbolBody(SymIndex);
    uint32_t Type = RI.getType(IsMips64EL);
    if (Body) {
      if (Target->relocNeedsPlt(Type, *Body)) {
        if (Body->isInPlt())
          continue;
        Out<ELFT>::Plt->addEntry(Body);
      }
      if (Target->relocNeedsGot(Type, *Body)) {
        if (Body->isInGot())
          continue;
        Out<ELFT>::Got->addEntry(Body);
      }
    }
    if (canBePreempted(Body)) {
      Body->setUsedInDynamicReloc();
      Out<ELFT>::RelaDyn->addReloc({C, RI});
    } else if (Config->Shared && !Target->isRelRelative(Type)) {
      Out<ELFT>::RelaDyn->addReloc({C, RI});
    }
  }
}

template <class ELFT>
void Writer<ELFT>::scanRelocs(const InputSection<ELFT> &C) {
  ObjectFile<ELFT> *File = C.getFile();
  ELFFile<ELFT> &EObj = File->getObj();

  if (!(C.getSectionHdr()->sh_flags & SHF_ALLOC))
    return;

  for (const Elf_Shdr *RelSec : C.RelocSections) {
    if (RelSec->sh_type == SHT_RELA)
      scanRelocs(C, EObj.relas(RelSec));
    else
      scanRelocs(C, EObj.rels(RelSec));
  }
}

template <class ELFT>
static void reportUndefined(const SymbolTable<ELFT> &S, const SymbolBody &Sym) {
  typedef typename ELFFile<ELFT>::Elf_Sym Elf_Sym;
  typedef typename ELFFile<ELFT>::Elf_Sym_Range Elf_Sym_Range;

  if (Config->Shared && !Config->NoUndefined)
    return;

  const Elf_Sym &SymE = cast<ELFSymbolBody<ELFT>>(Sym).Sym;
  ELFFileBase *SymFile = nullptr;

  for (const std::unique_ptr<ObjectFile<ELFT>> &File : S.getObjectFiles()) {
    Elf_Sym_Range Syms = File->getObj().symbols(File->getSymbolTable());
    if (&SymE > Syms.begin() && &SymE < Syms.end())
      SymFile = File.get();
  }

  std::string Message = "undefined symbol: " + Sym.getName().str();
  if (SymFile)
    Message += " in " + SymFile->getName().str();
  if (Config->NoInhibitExec)
    warning(Message);
  else
    error(Message);
}

// Local symbols are not in the linker's symbol table. This function scans
// each object file's symbol table to copy local symbols to the output.
template <class ELFT> void Writer<ELFT>::copyLocalSymbols() {
  for (const std::unique_ptr<ObjectFile<ELFT>> &F : Symtab.getObjectFiles()) {
    for (const Elf_Sym &Sym : F->getLocalSymbols()) {
      ErrorOr<StringRef> SymNameOrErr = Sym.getName(F->getStringTable());
      error(SymNameOrErr);
      StringRef SymName = *SymNameOrErr;
      if (!shouldKeepInSymtab<ELFT>(*F, SymName, Sym))
        continue;
      Out<ELFT>::SymTab->addSymbol(SymName, true);
    }
  }
}

// Output section ordering is determined by this function.
template <class ELFT>
static bool compareOutputSections(OutputSectionBase<ELFT::Is64Bits> *A,
                                  OutputSectionBase<ELFT::Is64Bits> *B) {
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;

  uintX_t AFlags = A->getFlags();
  uintX_t BFlags = B->getFlags();

  // Allocatable sections go first to reduce the total PT_LOAD size and
  // so debug info doesn't change addresses in actual code.
  bool AIsAlloc = AFlags & SHF_ALLOC;
  bool BIsAlloc = BFlags & SHF_ALLOC;
  if (AIsAlloc != BIsAlloc)
    return AIsAlloc;

  // We don't have any special requirements for the relative order of
  // two non allocatable sections.
  if (!AIsAlloc)
    return false;

  // We want the read only sections first so that they go in the PT_LOAD
  // covering the program headers at the start of the file.
  bool AIsWritable = AFlags & SHF_WRITE;
  bool BIsWritable = BFlags & SHF_WRITE;
  if (AIsWritable != BIsWritable)
    return BIsWritable;

  // For a corresponding reason, put non exec sections first (the program
  // header PT_LOAD is not executable).
  bool AIsExec = AFlags & SHF_EXECINSTR;
  bool BIsExec = BFlags & SHF_EXECINSTR;
  if (AIsExec != BIsExec)
    return BIsExec;

  // If we got here we know that both A and B and in the same PT_LOAD.
  // The last requirement we have is to put nobits section last. The
  // reason is that the only thing the dynamic linker will see about
  // them is a p_memsz that is larger than p_filesz. Seeing that it
  // zeros the end of the PT_LOAD, so that has to correspond to the
  // nobits sections.
  return A->getType() != SHT_NOBITS && B->getType() == SHT_NOBITS;
}

// Until this function is called, common symbols do not belong to any section.
// This function adds them to end of BSS section.
template <class ELFT>
static void addCommonSymbols(std::vector<DefinedCommon<ELFT> *> &Syms) {
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;
  typedef typename ELFFile<ELFT>::Elf_Sym Elf_Sym;

  // Sort the common symbols by alignment as an heuristic to pack them better.
  std::stable_sort(
    Syms.begin(), Syms.end(),
    [](const DefinedCommon<ELFT> *A, const DefinedCommon<ELFT> *B) {
      return A->MaxAlignment > B->MaxAlignment;
    });

  uintX_t Off = Out<ELFT>::Bss->getSize();
  for (DefinedCommon<ELFT> *C : Syms) {
    const Elf_Sym &Sym = C->Sym;
    uintX_t Align = C->MaxAlignment;
    Off = RoundUpToAlignment(Off, Align);
    C->OffsetInBSS = Off;
    Off += Sym.st_size;
  }

  Out<ELFT>::Bss->setSize(Off);
}

// Create output section objects and add them to OutputSections.
template <class ELFT> void Writer<ELFT>::createSections() {
  SmallDenseMap<SectionKey<ELFT::Is64Bits>, OutputSection<ELFT> *> Map;

  OutputSections.push_back(Out<ELFT>::Bss);
  Map[{Out<ELFT>::Bss->getName(), Out<ELFT>::Bss->getType(),
       Out<ELFT>::Bss->getFlags()}] = Out<ELFT>::Bss;

  // Declare linker generated symbols.
  // This must be done before the relocation scan to make sure we can correctly
  // decide if a dynamic relocation is needed or not.
  // FIXME: Make this more declarative.
  for (StringRef Name :
       {"__preinit_array_start", "__preinit_array_end", "__init_array_start",
        "__init_array_end", "__fini_array_start", "__fini_array_end"})
    Symtab.addIgnoredSym(Name);

  // __tls_get_addr is defined by the dynamic linker for dynamic ELFs. For
  // static linking the linker is required to optimize away any references to
  // __tls_get_addr, so it's not defined anywhere. Create a hidden definition
  // to avoid the undefined symbol error.
  if (!isOutputDynamic())
    Symtab.addIgnoredSym("__tls_get_addr");

  for (const std::unique_ptr<ObjectFile<ELFT>> &F : Symtab.getObjectFiles()) {
    for (InputSection<ELFT> *C : F->getSections()) {
      if (!C || C == &InputSection<ELFT>::Discarded)
        continue;
      const Elf_Shdr *H = C->getSectionHdr();
      uintX_t OutFlags = H->sh_flags & ~SHF_GROUP;
      SectionKey<ELFT::Is64Bits> Key{C->getSectionName(), H->sh_type, OutFlags};
      OutputSection<ELFT> *&Sec = Map[Key];
      if (!Sec) {
        Sec = new (CAlloc.Allocate())
            OutputSection<ELFT>(Key.Name, Key.Type, Key.Flags);
        OutputSections.push_back(Sec);
      }
      Sec->addSection(C);
      scanRelocs(*C);
    }
  }

  Out<ELFT>::Dynamic->PreInitArraySec =
      Map.lookup({".preinit_array", SHT_PREINIT_ARRAY, SHF_WRITE | SHF_ALLOC});
  Out<ELFT>::Dynamic->InitArraySec =
      Map.lookup({".init_array", SHT_INIT_ARRAY, SHF_WRITE | SHF_ALLOC});
  Out<ELFT>::Dynamic->FiniArraySec =
      Map.lookup({".fini_array", SHT_FINI_ARRAY, SHF_WRITE | SHF_ALLOC});

  auto AddStartEnd = [&](StringRef Start, StringRef End,
                         OutputSection<ELFT> *OS) {
    if (OS) {
      Symtab.addSyntheticSym(Start, *OS, 0);
      Symtab.addSyntheticSym(End, *OS, OS->getSize());
    }
  };

  AddStartEnd("__preinit_array_start", "__preinit_array_end",
              Out<ELFT>::Dynamic->PreInitArraySec);
  AddStartEnd("__init_array_start", "__init_array_end",
              Out<ELFT>::Dynamic->InitArraySec);
  AddStartEnd("__fini_array_start", "__fini_array_end",
              Out<ELFT>::Dynamic->FiniArraySec);

  // FIXME: Try to avoid the extra walk over all global symbols.
  std::vector<DefinedCommon<ELFT> *> CommonSymbols;
  for (auto &P : Symtab.getSymbols()) {
    StringRef Name = P.first;
    SymbolBody *Body = P.second->Body;
    if (auto *U = dyn_cast<Undefined<ELFT>>(Body)) {
      if (!U->isWeak() && !U->canKeepUndefined())
        reportUndefined<ELFT>(Symtab, *Body);
    }

    if (auto *C = dyn_cast<DefinedCommon<ELFT>>(Body))
      CommonSymbols.push_back(C);
    if (!includeInSymtab<ELFT>(*Body))
      continue;
    Out<ELFT>::SymTab->addSymbol(Name);

    if (needsDynamicSections() && includeInDynamicSymtab(*Body))
      Out<ELFT>::HashTab->addSymbol(Body);
  }
  addCommonSymbols(CommonSymbols);

  OutputSections.push_back(Out<ELFT>::SymTab);
  if (needsDynamicSections()) {
    if (needsInterpSection())
      OutputSections.push_back(Out<ELFT>::Interp);
    OutputSections.push_back(Out<ELFT>::DynSymTab);
    OutputSections.push_back(Out<ELFT>::HashTab);
    OutputSections.push_back(Out<ELFT>::Dynamic);
    OutputSections.push_back(Out<ELFT>::DynStrTab);
    if (Out<ELFT>::RelaDyn->hasRelocs())
      OutputSections.push_back(Out<ELFT>::RelaDyn);
  }
  if (!Out<ELFT>::Got->empty())
    OutputSections.push_back(Out<ELFT>::Got);
  if (!Out<ELFT>::Plt->empty())
    OutputSections.push_back(Out<ELFT>::Plt);

  std::stable_sort(OutputSections.begin(), OutputSections.end(),
                   compareOutputSections<ELFT>);

  // Always put StrTabSec last so that no section names are added to it after
  // it's finalized.
  OutputSections.push_back(Out<ELFT>::StrTab);

  for (unsigned I = 0, N = OutputSections.size(); I < N; ++I)
    OutputSections[I]->setSectionIndex(I + 1);

  // Fill the DynStrTab early.
  Out<ELFT>::Dynamic->finalize();

  // Fix each section's header (e.g. sh_size, sh_link, etc.)
  for (OutputSectionBase<ELFT::Is64Bits> *Sec : OutputSections) {
    Out<ELFT>::StrTab->add(Sec->getName());
    Sec->finalize();
  }
}

template <class ELFT>
static bool needsPHDR(OutputSectionBase<ELFT::Is64Bits> *Sec) {
  return Sec->getFlags() & SHF_ALLOC;
}

// Visits all sections to assign incremental, non-overlapping RVAs and
// file offsets.
template <class ELFT> void Writer<ELFT>::assignAddresses() {
  assert(!OutputSections.empty() && "No output sections to layout!");
  uintX_t VA = getVAStart();
  uintX_t FileOff = 0;

  FileOff += sizeof(Elf_Ehdr);
  VA += sizeof(Elf_Ehdr);

  // The first PHDR entry is PT_PHDR which describes the program header itself.
  PHDRs.push_back(&PhdrPHDR);
  PhdrPHDR.Header.p_align = 8;
  PhdrPHDR.Header.p_offset = FileOff;
  PhdrPHDR.Header.p_vaddr = VA;
  PhdrPHDR.Header.p_paddr = VA;

  // Reserve space for PHDRs.
  ProgramHeaderOff = FileOff;
  FileOff = RoundUpToAlignment(FileOff, Target->getPageSize());
  VA = RoundUpToAlignment(VA, Target->getPageSize());

  if (needsInterpSection())
    PHDRs.push_back(&InterpPHDR);

  // Create a PHDR for the file header.
  PHDRs.push_back(&FileHeaderPHDR);
  FileHeaderPHDR.Header.p_vaddr = getVAStart();
  FileHeaderPHDR.Header.p_paddr = getVAStart();
  FileHeaderPHDR.Header.p_align = Target->getPageSize();

  for (OutputSectionBase<ELFT::Is64Bits> *Sec : OutputSections) {
    if (Sec->getSize()) {
      uintX_t Flags = toPHDRFlags(Sec->getFlags());
      ProgramHeader<ELFT> *Last = PHDRs.back();
      if (Last->Header.p_flags != Flags || !needsPHDR<ELFT>(Sec)) {
        // Flags changed. End current PHDR and potentially create a new one.
        if (!Last->Closed) {
          Last->Header.p_filesz = FileOff - Last->Header.p_offset;
          Last->Header.p_memsz = VA - Last->Header.p_vaddr;
          Last->Closed = true;
        }

        if (needsPHDR<ELFT>(Sec)) {
          VA = RoundUpToAlignment(VA, Target->getPageSize());
          FileOff = RoundUpToAlignment(FileOff, Target->getPageSize());
          PHDRs.push_back(new (PAlloc)
                              ProgramHeader<ELFT>(PT_LOAD, Flags, FileOff, VA));
        }
      }
    }

    uintX_t Align = Sec->getAlign();
    uintX_t Size = Sec->getSize();
    if (Sec->getFlags() & SHF_ALLOC) {
      VA = RoundUpToAlignment(VA, Align);
      Sec->setVA(VA);
      VA += Size;
    }
    FileOff = RoundUpToAlignment(FileOff, Align);
    Sec->setFileOffset(FileOff);
    if (Sec->getType() != SHT_NOBITS)
      FileOff += Size;
  }

  // Add a PHDR for the dynamic table.
  if (needsDynamicSections())
    PHDRs.push_back(&DynamicPHDR);

  FileOff += OffsetToAlignment(FileOff, ELFT::Is64Bits ? 8 : 4);

  // Fix up the first entry's size.
  PhdrPHDR.Header.p_filesz = sizeof(Elf_Phdr) * PHDRs.size();
  PhdrPHDR.Header.p_memsz = sizeof(Elf_Phdr) * PHDRs.size();

  // Add space for section headers.
  SectionHeaderOff = FileOff;
  FileOff += getNumSections() * sizeof(Elf_Shdr);
  FileSize = FileOff;
}

template <class ELFT> void Writer<ELFT>::writeHeader() {
  uint8_t *Buf = Buffer->getBufferStart();
  auto *EHdr = reinterpret_cast<Elf_Ehdr *>(Buf);
  EHdr->e_ident[EI_MAG0] = 0x7F;
  EHdr->e_ident[EI_MAG1] = 0x45;
  EHdr->e_ident[EI_MAG2] = 0x4C;
  EHdr->e_ident[EI_MAG3] = 0x46;
  EHdr->e_ident[EI_CLASS] = ELFT::Is64Bits ? ELFCLASS64 : ELFCLASS32;
  EHdr->e_ident[EI_DATA] = ELFT::TargetEndianness == llvm::support::little
                               ? ELFDATA2LSB
                               : ELFDATA2MSB;
  EHdr->e_ident[EI_VERSION] = EV_CURRENT;

  auto &FirstObj = cast<ObjectFile<ELFT>>(*Symtab.getFirstELF());
  EHdr->e_ident[EI_OSABI] = FirstObj.getOSABI();

  // FIXME: Generalize the segment construction similar to how we create
  // output sections.

  EHdr->e_type = Config->Shared ? ET_DYN : ET_EXEC;
  EHdr->e_machine = FirstObj.getEMachine();
  EHdr->e_version = EV_CURRENT;
  SymbolBody *Entry = Symtab.getEntrySym();
  EHdr->e_entry = Entry ? getSymVA<ELFT>(cast<ELFSymbolBody<ELFT>>(*Entry)) : 0;
  EHdr->e_phoff = ProgramHeaderOff;
  EHdr->e_shoff = SectionHeaderOff;
  EHdr->e_ehsize = sizeof(Elf_Ehdr);
  EHdr->e_phentsize = sizeof(Elf_Phdr);
  EHdr->e_phnum = PHDRs.size();
  EHdr->e_shentsize = sizeof(Elf_Shdr);
  EHdr->e_shnum = getNumSections();
  EHdr->e_shstrndx = Out<ELFT>::StrTab->getSectionIndex();

  // If nothing was merged into the file header PT_LOAD, set the size correctly.
  if (FileHeaderPHDR.Header.p_filesz == Target->getPageSize()) {
    uint64_t Size = sizeof(Elf_Ehdr) + sizeof(Elf_Phdr) * PHDRs.size();
    FileHeaderPHDR.Header.p_filesz = Size;
    FileHeaderPHDR.Header.p_memsz = Size;
  }

  if (needsInterpSection())
    InterpPHDR.setValuesFromSection(Out<ELFT>::Interp);
  if (needsDynamicSections())
    DynamicPHDR.setValuesFromSection(Out<ELFT>::Dynamic);

  auto PHdrs = reinterpret_cast<Elf_Phdr *>(Buf + EHdr->e_phoff);
  for (ProgramHeader<ELFT> *PHDR : PHDRs)
    *PHdrs++ = PHDR->Header;

  auto SHdrs = reinterpret_cast<Elf_Shdr *>(Buf + EHdr->e_shoff);
  // First entry is null.
  ++SHdrs;
  for (OutputSectionBase<ELFT::Is64Bits> *Sec : OutputSections) {
    Sec->setNameOffset(Out<ELFT>::StrTab->getFileOff(Sec->getName()));
    Sec->template writeHeaderTo<ELFT::TargetEndianness>(SHdrs++);
  }
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
  for (OutputSectionBase<ELFT::Is64Bits> *Sec : OutputSections)
    Sec->writeTo(Buf + Sec->getFileOff());
}

template void lld::elf2::writeResult<ELF32LE>(SymbolTable<ELF32LE> *Symtab);
template void lld::elf2::writeResult<ELF32BE>(SymbolTable<ELF32BE> *Symtab);
template void lld::elf2::writeResult<ELF64LE>(SymbolTable<ELF64LE> *Symtab);
template void lld::elf2::writeResult<ELF64BE>(SymbolTable<ELF64BE> *Symtab);
