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
#include "LinkerScript.h"
#include "Memory.h"
#include "OutputSections.h"
#include "Relocations.h"
#include "Strings.h"
#include "SymbolTable.h"
#include "SyntheticSections.h"
#include "Target.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <climits>

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace llvm::support;
using namespace llvm::support::endian;

using namespace lld;
using namespace lld::elf;

namespace {
// The writer writes a SymbolTable result to a file.
template <class ELFT> class Writer {
public:
  typedef typename ELFT::uint uintX_t;
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::Ehdr Elf_Ehdr;
  typedef typename ELFT::Phdr Elf_Phdr;
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::SymRange Elf_Sym_Range;
  typedef typename ELFT::Rela Elf_Rela;
  void run();

private:
  typedef PhdrEntry<ELFT> Phdr;

  void copyLocalSymbols();
  void addReservedSymbols();
  void addInputSec(InputSectionBase<ELFT> *S);
  void createSections();
  void forEachRelSec(std::function<void(InputSectionBase<ELFT> &,
                                        const typename ELFT::Shdr &)> Fn);
  void sortSections();
  void finalizeSections();
  void addPredefinedSections();
  bool needsGot();

  std::vector<Phdr> createPhdrs();
  void assignAddresses();
  void assignFileOffsets();
  void assignFileOffsetsBinary();
  void setPhdrs();
  void fixHeaders();
  void fixSectionAlignments();
  void fixAbsoluteSymbols();
  void openFile();
  void writeHeader();
  void writeSections();
  void writeSectionsBinary();
  void writeBuildId();

  std::unique_ptr<FileOutputBuffer> Buffer;

  std::vector<OutputSectionBase<ELFT> *> OutputSections;
  OutputSectionFactory<ELFT> Factory;

  void addRelIpltSymbols();
  void addStartEndSymbols();
  void addStartStopSymbols(OutputSectionBase<ELFT> *Sec);
  OutputSectionBase<ELFT> *findSection(StringRef Name);

  std::vector<Phdr> Phdrs;

  uintX_t FileSize;
  uintX_t SectionHeaderOff;
};
} // anonymous namespace

StringRef elf::getOutputSectionName(StringRef Name) {
  if (Config->Relocatable)
    return Name;

  for (StringRef V :
       {".text.", ".rodata.", ".data.rel.ro.", ".data.", ".bss.",
        ".init_array.", ".fini_array.", ".ctors.", ".dtors.", ".tbss.",
        ".gcc_except_table.", ".tdata.", ".ARM.exidx."}) {
    StringRef Prefix = V.drop_back();
    if (Name.startswith(V) || Name == Prefix)
      return Prefix;
  }

  // ".zdebug_" is a prefix for ZLIB-compressed sections.
  // Because we decompressed input sections, we want to remove 'z'.
  if (Name.startswith(".zdebug_"))
    return Saver.save(Twine(".") + Name.substr(2));
  return Name;
}

template <class ELFT> void elf::reportDiscarded(InputSectionBase<ELFT> *IS) {
  if (!Config->PrintGcSections || !IS || IS == &InputSection<ELFT>::Discarded ||
      IS->Live)
    return;
  errs() << "removing unused section from '" << IS->Name << "' in file '"
         << IS->getFile()->getName() << "'\n";
}

template <class ELFT> static bool needsInterpSection() {
  return !Symtab<ELFT>::X->getSharedFiles().empty() &&
         !Config->DynamicLinker.empty() &&
         !Script<ELFT>::X->ignoreInterpSection();
}

template <class ELFT> void elf::writeResult() {
  typedef typename ELFT::uint uintX_t;
  typedef typename ELFT::Ehdr Elf_Ehdr;

  // Create singleton output sections.
  OutputSection<ELFT> Bss(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
  DynamicSection<ELFT> Dynamic;
  EhOutputSection<ELFT> EhFrame;
  GotSection<ELFT> Got;
  PltSection<ELFT> Plt;
  RelocationSection<ELFT> RelaDyn(Config->Rela ? ".rela.dyn" : ".rel.dyn",
                                  Config->ZCombreloc);
  StringTableSection<ELFT> ShStrTab(".shstrtab", false);
  VersionTableSection<ELFT> VerSym;
  VersionNeedSection<ELFT> VerNeed;

  OutputSectionBase<ELFT> ElfHeader("", 0, SHF_ALLOC);
  ElfHeader.setSize(sizeof(Elf_Ehdr));
  OutputSectionBase<ELFT> ProgramHeaders("", 0, SHF_ALLOC);
  ProgramHeaders.updateAlignment(sizeof(uintX_t));

  // Instantiate optional output sections if they are needed.
  std::unique_ptr<InterpSection<ELFT>> Interp;
  std::unique_ptr<BuildIdSection<ELFT>> BuildId;
  std::unique_ptr<StringTableSection<ELFT>> DynStrTab;
  std::unique_ptr<SymbolTableSection<ELFT>> DynSymTab;
  std::unique_ptr<EhFrameHeader<ELFT>> EhFrameHdr;
  std::unique_ptr<GdbIndexSection<ELFT>> GdbIndex;
  std::unique_ptr<GnuHashTableSection<ELFT>> GnuHashTab;
  std::unique_ptr<GotPltSection<ELFT>> GotPlt;
  std::unique_ptr<HashTableSection<ELFT>> HashTab;
  std::unique_ptr<RelocationSection<ELFT>> RelaPlt;
  std::unique_ptr<StringTableSection<ELFT>> StrTab;
  std::unique_ptr<SymbolTableSection<ELFT>> SymTabSec;
  std::unique_ptr<OutputSection<ELFT>> MipsRldMap;
  std::unique_ptr<VersionDefinitionSection<ELFT>> VerDef;

  if (needsInterpSection<ELFT>())
    Interp.reset(new InterpSection<ELFT>);

  if (Config->BuildId == BuildIdKind::Fast)
    BuildId.reset(new BuildIdFastHash<ELFT>);
  else if (Config->BuildId == BuildIdKind::Md5)
    BuildId.reset(new BuildIdMd5<ELFT>);
  else if (Config->BuildId == BuildIdKind::Sha1)
    BuildId.reset(new BuildIdSha1<ELFT>);
  else if (Config->BuildId == BuildIdKind::Uuid)
    BuildId.reset(new BuildIdUuid<ELFT>);
  else if (Config->BuildId == BuildIdKind::Hexstring)
    BuildId.reset(new BuildIdHexstring<ELFT>);

  if (!Symtab<ELFT>::X->getSharedFiles().empty() || Config->Pic) {
    DynStrTab.reset(new StringTableSection<ELFT>(".dynstr", true));
    DynSymTab.reset(new SymbolTableSection<ELFT>(*DynStrTab));
  }

  if (Config->EhFrameHdr)
    EhFrameHdr.reset(new EhFrameHeader<ELFT>);

  if (Config->GnuHash)
    GnuHashTab.reset(new GnuHashTableSection<ELFT>);
  if (Config->SysvHash)
    HashTab.reset(new HashTableSection<ELFT>);
  if (Config->GdbIndex)
    GdbIndex.reset(new GdbIndexSection<ELFT>);
  StringRef S = Config->Rela ? ".rela.plt" : ".rel.plt";
  GotPlt.reset(new GotPltSection<ELFT>);
  RelaPlt.reset(new RelocationSection<ELFT>(S, false /*Sort*/));
  if (Config->Strip != StripPolicy::All) {
    StrTab.reset(new StringTableSection<ELFT>(".strtab", false));
    SymTabSec.reset(new SymbolTableSection<ELFT>(*StrTab));
  }
  if (Config->EMachine == EM_MIPS && !Config->Shared) {
    // This is a MIPS specific section to hold a space within the data segment
    // of executable file which is pointed to by the DT_MIPS_RLD_MAP entry.
    // See "Dynamic section" in Chapter 5 in the following document:
    // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
    MipsRldMap.reset(new OutputSection<ELFT>(".rld_map", SHT_PROGBITS,
                                             SHF_ALLOC | SHF_WRITE));
    MipsRldMap->setSize(sizeof(uintX_t));
    MipsRldMap->updateAlignment(sizeof(uintX_t));
  }
  if (!Config->VersionDefinitions.empty())
    VerDef.reset(new VersionDefinitionSection<ELFT>());

  Out<ELFT>::Bss = &Bss;
  Out<ELFT>::DynStrTab = DynStrTab.get();
  Out<ELFT>::DynSymTab = DynSymTab.get();
  Out<ELFT>::Dynamic = &Dynamic;
  Out<ELFT>::EhFrame = &EhFrame;
  Out<ELFT>::EhFrameHdr = EhFrameHdr.get();
  Out<ELFT>::GdbIndex = GdbIndex.get();
  Out<ELFT>::GnuHashTab = GnuHashTab.get();
  Out<ELFT>::Got = &Got;
  Out<ELFT>::GotPlt = GotPlt.get();
  Out<ELFT>::HashTab = HashTab.get();
  Out<ELFT>::Interp = Interp.get();
  Out<ELFT>::Plt = &Plt;
  Out<ELFT>::RelaDyn = &RelaDyn;
  Out<ELFT>::RelaPlt = RelaPlt.get();
  Out<ELFT>::ShStrTab = &ShStrTab;
  Out<ELFT>::StrTab = StrTab.get();
  Out<ELFT>::SymTab = SymTabSec.get();
  Out<ELFT>::VerDef = VerDef.get();
  Out<ELFT>::VerSym = &VerSym;
  Out<ELFT>::VerNeed = &VerNeed;
  Out<ELFT>::MipsRldMap = MipsRldMap.get();
  Out<ELFT>::Opd = nullptr;
  Out<ELFT>::OpdBuf = nullptr;
  Out<ELFT>::TlsPhdr = nullptr;
  Out<ELFT>::ElfHeader = &ElfHeader;
  Out<ELFT>::ProgramHeaders = &ProgramHeaders;

  Out<ELFT>::PreinitArray = nullptr;
  Out<ELFT>::InitArray = nullptr;
  Out<ELFT>::FiniArray = nullptr;

  // Initialize linker generated sections
  In<ELFT>::BuildId = BuildId.get();
  In<ELFT>::Sections = {BuildId.get()};

  Writer<ELFT>().run();
}

template <class ELFT> static std::vector<DefinedCommon *> getCommonSymbols() {
  std::vector<DefinedCommon *> V;
  for (Symbol *S : Symtab<ELFT>::X->getSymbols())
    if (auto *B = dyn_cast<DefinedCommon>(S->body()))
      V.push_back(B);
  return V;
}

// The main function of the writer.
template <class ELFT> void Writer<ELFT>::run() {
  addReservedSymbols();

  if (Target->NeedsThunks)
    forEachRelSec(createThunks<ELFT>);

  InputSection<ELFT> Common =
      InputSection<ELFT>::createCommonInputSection(getCommonSymbols<ELFT>());
  InputSection<ELFT>::CommonInputSection = &Common;

  Script<ELFT>::X->OutputSections = &OutputSections;
  if (ScriptConfig->HasSections) {
    Script<ELFT>::X->createSections(Factory);
  } else {
    createSections();
    Script<ELFT>::X->processCommands(Factory);
  }

  if (Config->Discard != DiscardPolicy::All)
    copyLocalSymbols();

  finalizeSections();
  if (HasError)
    return;

  if (Config->Relocatable) {
    assignFileOffsets();
  } else {
    Phdrs = Script<ELFT>::X->hasPhdrsCommands() ? Script<ELFT>::X->createPhdrs()
                                                : createPhdrs();
    fixHeaders();
    if (ScriptConfig->HasSections) {
      Script<ELFT>::X->assignAddresses(Phdrs);
    } else {
      fixSectionAlignments();
      assignAddresses();
    }

    if (!Config->OFormatBinary)
      assignFileOffsets();
    else
      assignFileOffsetsBinary();

    setPhdrs();
    fixAbsoluteSymbols();
  }

  openFile();
  if (HasError)
    return;
  if (!Config->OFormatBinary) {
    writeHeader();
    writeSections();
  } else {
    writeSectionsBinary();
  }
  writeBuildId();
  if (HasError)
    return;
  if (auto EC = Buffer->commit())
    error(EC, "failed to write to the output file");
  if (Config->ExitEarly) {
    // Flush the output streams and exit immediately.  A full shutdown is a good
    // test that we are keeping track of all allocated memory, but actually
    // freeing it is a waste of time in a regular linker run.
    exitLld(0);
  }
}

template <class ELFT>
static bool shouldKeepInSymtab(InputSectionBase<ELFT> *Sec, StringRef SymName,
                               const SymbolBody &B) {
  if (B.isFile())
    return false;

  // We keep sections in symtab for relocatable output.
  if (B.isSection())
    return Config->Relocatable;

  // If sym references a section in a discarded group, don't keep it.
  if (Sec == &InputSection<ELFT>::Discarded)
    return false;

  if (Config->Discard == DiscardPolicy::None)
    return true;

  // In ELF assembly .L symbols are normally discarded by the assembler.
  // If the assembler fails to do so, the linker discards them if
  // * --discard-locals is used.
  // * The symbol is in a SHF_MERGE section, which is normally the reason for
  //   the assembler keeping the .L symbol.
  if (!SymName.startswith(".L") && !SymName.empty())
    return true;

  if (Config->Discard == DiscardPolicy::Locals)
    return false;

  return !Sec || !(Sec->Flags & SHF_MERGE);
}

template <class ELFT> static bool includeInSymtab(const SymbolBody &B) {
  if (!B.isLocal() && !B.symbol()->IsUsedInRegularObj)
    return false;

  if (auto *D = dyn_cast<DefinedRegular<ELFT>>(&B)) {
    // Always include absolute symbols.
    if (!D->Section)
      return true;
    // Exclude symbols pointing to garbage-collected sections.
    if (!D->Section->Live)
      return false;
    if (auto *S = dyn_cast<MergeInputSection<ELFT>>(D->Section))
      if (!S->getSectionPiece(D->Value)->Live)
        return false;
  }
  return true;
}

// Local symbols are not in the linker's symbol table. This function scans
// each object file's symbol table to copy local symbols to the output.
template <class ELFT> void Writer<ELFT>::copyLocalSymbols() {
  if (!Out<ELFT>::SymTab)
    return;
  for (elf::ObjectFile<ELFT> *F : Symtab<ELFT>::X->getObjectFiles()) {
    StringRef StrTab = F->getStringTable();
    for (SymbolBody *B : F->getLocalSymbols()) {
      if (!B->IsLocal)
        fatal(getFilename(F) +
              ": broken object: getLocalSymbols returns a non-local symbol");
      auto *DR = dyn_cast<DefinedRegular<ELFT>>(B);
      // No reason to keep local undefined symbol in symtab.
      if (!DR)
        continue;
      if (!includeInSymtab<ELFT>(*B))
        continue;
      if (B->getNameOffset() >= StrTab.size())
        fatal(getFilename(F) + ": invalid symbol name offset");
      StringRef SymName(StrTab.data() + B->getNameOffset());
      InputSectionBase<ELFT> *Sec = DR->Section;
      if (!shouldKeepInSymtab<ELFT>(Sec, SymName, *B))
        continue;
      ++Out<ELFT>::SymTab->NumLocals;
      if (Config->Relocatable)
        B->DynsymIndex = Out<ELFT>::SymTab->NumLocals;
      F->KeptLocalSyms.push_back(
          std::make_pair(DR, Out<ELFT>::SymTab->StrTabSec.addString(SymName)));
    }
  }
}

// PPC64 has a number of special SHT_PROGBITS+SHF_ALLOC+SHF_WRITE sections that
// we would like to make sure appear is a specific order to maximize their
// coverage by a single signed 16-bit offset from the TOC base pointer.
// Conversely, the special .tocbss section should be first among all SHT_NOBITS
// sections. This will put it next to the loaded special PPC64 sections (and,
// thus, within reach of the TOC base pointer).
static int getPPC64SectionRank(StringRef SectionName) {
  return StringSwitch<int>(SectionName)
      .Case(".tocbss", 0)
      .Case(".branch_lt", 2)
      .Case(".toc", 3)
      .Case(".toc1", 4)
      .Case(".opd", 5)
      .Default(1);
}

template <class ELFT> bool elf::isRelroSection(OutputSectionBase<ELFT> *Sec) {
  if (!Config->ZRelro)
    return false;
  typename ELFT::uint Flags = Sec->getFlags();
  if (!(Flags & SHF_ALLOC) || !(Flags & SHF_WRITE))
    return false;
  if (Flags & SHF_TLS)
    return true;
  uint32_t Type = Sec->getType();
  if (Type == SHT_INIT_ARRAY || Type == SHT_FINI_ARRAY ||
      Type == SHT_PREINIT_ARRAY)
    return true;
  if (Sec == Out<ELFT>::GotPlt)
    return Config->ZNow;
  if (Sec == Out<ELFT>::Dynamic || Sec == Out<ELFT>::Got)
    return true;
  StringRef S = Sec->getName();
  return S == ".data.rel.ro" || S == ".ctors" || S == ".dtors" || S == ".jcr" ||
         S == ".eh_frame";
}

template <class ELFT>
static bool compareSectionsNonScript(OutputSectionBase<ELFT> *A,
                                     OutputSectionBase<ELFT> *B) {
  typedef typename ELFT::uint uintX_t;
  uintX_t AFlags = A->getFlags();
  uintX_t BFlags = B->getFlags();

  // Allocatable sections go first to reduce the total PT_LOAD size and
  // so debug info doesn't change addresses in actual code.
  bool AIsAlloc = AFlags & SHF_ALLOC;
  bool BIsAlloc = BFlags & SHF_ALLOC;
  if (AIsAlloc != BIsAlloc)
    return AIsAlloc;

  // We don't have any special requirements for the relative order of two non
  // allocatable sections.
  if (!AIsAlloc)
    return false;

  // We want the read only sections first so that they go in the PT_LOAD
  // covering the program headers at the start of the file.
  bool AIsWritable = AFlags & SHF_WRITE;
  bool BIsWritable = BFlags & SHF_WRITE;
  if (AIsWritable != BIsWritable)
    return BIsWritable;

  if (!ScriptConfig->HasSections) {
    // For a corresponding reason, put non exec sections first (the program
    // header PT_LOAD is not executable).
    // We only do that if we are not using linker scripts, since with linker
    // scripts ro and rx sections are in the same PT_LOAD, so their relative
    // order is not important.
    bool AIsExec = AFlags & SHF_EXECINSTR;
    bool BIsExec = BFlags & SHF_EXECINSTR;
    if (AIsExec != BIsExec)
      return BIsExec;
  }

  // If we got here we know that both A and B are in the same PT_LOAD.

  // The TLS initialization block needs to be a single contiguous block in a R/W
  // PT_LOAD, so stick TLS sections directly before R/W sections. The TLS NOBITS
  // sections are placed here as they don't take up virtual address space in the
  // PT_LOAD.
  bool AIsTls = AFlags & SHF_TLS;
  bool BIsTls = BFlags & SHF_TLS;
  if (AIsTls != BIsTls)
    return AIsTls;

  // The next requirement we have is to put nobits sections last. The
  // reason is that the only thing the dynamic linker will see about
  // them is a p_memsz that is larger than p_filesz. Seeing that it
  // zeros the end of the PT_LOAD, so that has to correspond to the
  // nobits sections.
  bool AIsNoBits = A->getType() == SHT_NOBITS;
  bool BIsNoBits = B->getType() == SHT_NOBITS;
  if (AIsNoBits != BIsNoBits)
    return BIsNoBits;

  // We place RelRo section before plain r/w ones.
  bool AIsRelRo = isRelroSection(A);
  bool BIsRelRo = isRelroSection(B);
  if (AIsRelRo != BIsRelRo)
    return AIsRelRo;

  // Some architectures have additional ordering restrictions for sections
  // within the same PT_LOAD.
  if (Config->EMachine == EM_PPC64)
    return getPPC64SectionRank(A->getName()) <
           getPPC64SectionRank(B->getName());

  return false;
}

// Output section ordering is determined by this function.
template <class ELFT>
static bool compareSections(OutputSectionBase<ELFT> *A,
                            OutputSectionBase<ELFT> *B) {
  // For now, put sections mentioned in a linker script first.
  int AIndex = Script<ELFT>::X->getSectionIndex(A->getName());
  int BIndex = Script<ELFT>::X->getSectionIndex(B->getName());
  bool AInScript = AIndex != INT_MAX;
  bool BInScript = BIndex != INT_MAX;
  if (AInScript != BInScript)
    return AInScript;
  // If both are in the script, use that order.
  if (AInScript)
    return AIndex < BIndex;

  return compareSectionsNonScript(A, B);
}

template <class ELFT> static bool isDiscarded(InputSectionBase<ELFT> *S) {
  return !S || S == &InputSection<ELFT>::Discarded || !S->Live;
}

// Program header entry
template <class ELFT>
PhdrEntry<ELFT>::PhdrEntry(unsigned Type, unsigned Flags) {
  H.p_type = Type;
  H.p_flags = Flags;
}

template <class ELFT> void PhdrEntry<ELFT>::add(OutputSectionBase<ELFT> *Sec) {
  Last = Sec;
  if (!First)
    First = Sec;
  H.p_align = std::max<typename ELFT::uint>(H.p_align, Sec->getAlignment());
  if (H.p_type == PT_LOAD)
    Sec->FirstInPtLoad = First;
}

template <class ELFT>
static Symbol *
addOptionalSynthetic(StringRef Name, OutputSectionBase<ELFT> *Sec,
                     typename ELFT::uint Val, uint8_t StOther = STV_HIDDEN) {
  SymbolBody *S = Symtab<ELFT>::X->find(Name);
  if (!S)
    return nullptr;
  if (!S->isUndefined() && !S->isShared())
    return S->symbol();
  return Symtab<ELFT>::X->addSynthetic(Name, Sec, Val, StOther);
}

// The beginning and the ending of .rel[a].plt section are marked
// with __rel[a]_iplt_{start,end} symbols if it is a statically linked
// executable. The runtime needs these symbols in order to resolve
// all IRELATIVE relocs on startup. For dynamic executables, we don't
// need these symbols, since IRELATIVE relocs are resolved through GOT
// and PLT. For details, see http://www.airs.com/blog/archives/403.
template <class ELFT> void Writer<ELFT>::addRelIpltSymbols() {
  if (Out<ELFT>::DynSymTab || !Out<ELFT>::RelaPlt)
    return;
  StringRef S = Config->Rela ? "__rela_iplt_start" : "__rel_iplt_start";
  addOptionalSynthetic(S, Out<ELFT>::RelaPlt, 0);

  S = Config->Rela ? "__rela_iplt_end" : "__rel_iplt_end";
  addOptionalSynthetic(S, Out<ELFT>::RelaPlt,
                       DefinedSynthetic<ELFT>::SectionEnd);
}

// The linker is expected to define some symbols depending on
// the linking result. This function defines such symbols.
template <class ELFT> void Writer<ELFT>::addReservedSymbols() {
  if (Config->EMachine == EM_MIPS && !Config->Relocatable) {
    // Define _gp for MIPS. st_value of _gp symbol will be updated by Writer
    // so that it points to an absolute address which is relative to GOT.
    // See "Global Data Symbols" in Chapter 6 in the following document:
    // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
    Symtab<ELFT>::X->addSynthetic("_gp", Out<ELFT>::Got, MipsGPOffset,
                                  STV_HIDDEN);

    // On MIPS O32 ABI, _gp_disp is a magic symbol designates offset between
    // start of function and 'gp' pointer into GOT.
    Symbol *Sym =
        addOptionalSynthetic("_gp_disp", Out<ELFT>::Got, MipsGPOffset);
    if (Sym)
      ElfSym<ELFT>::MipsGpDisp = Sym->body();

    // The __gnu_local_gp is a magic symbol equal to the current value of 'gp'
    // pointer. This symbol is used in the code generated by .cpload pseudo-op
    // in case of using -mno-shared option.
    // https://sourceware.org/ml/binutils/2004-12/msg00094.html
    addOptionalSynthetic("__gnu_local_gp", Out<ELFT>::Got, MipsGPOffset);
  }

  // In the assembly for 32 bit x86 the _GLOBAL_OFFSET_TABLE_ symbol
  // is magical and is used to produce a R_386_GOTPC relocation.
  // The R_386_GOTPC relocation value doesn't actually depend on the
  // symbol value, so it could use an index of STN_UNDEF which, according
  // to the spec, means the symbol value is 0.
  // Unfortunately both gas and MC keep the _GLOBAL_OFFSET_TABLE_ symbol in
  // the object file.
  // The situation is even stranger on x86_64 where the assembly doesn't
  // need the magical symbol, but gas still puts _GLOBAL_OFFSET_TABLE_ as
  // an undefined symbol in the .o files.
  // Given that the symbol is effectively unused, we just create a dummy
  // hidden one to avoid the undefined symbol error.
  if (!Config->Relocatable)
    Symtab<ELFT>::X->addIgnored("_GLOBAL_OFFSET_TABLE_");

  // __tls_get_addr is defined by the dynamic linker for dynamic ELFs. For
  // static linking the linker is required to optimize away any references to
  // __tls_get_addr, so it's not defined anywhere. Create a hidden definition
  // to avoid the undefined symbol error. As usual special cases are ARM and
  // MIPS - the libc for these targets defines __tls_get_addr itself because
  // there are no TLS optimizations for these targets.
  if (!Out<ELFT>::DynSymTab &&
      (Config->EMachine != EM_MIPS && Config->EMachine != EM_ARM))
    Symtab<ELFT>::X->addIgnored("__tls_get_addr");

  // If linker script do layout we do not need to create any standart symbols.
  if (ScriptConfig->HasSections)
    return;

  ElfSym<ELFT>::EhdrStart = Symtab<ELFT>::X->addIgnored("__ehdr_start");

  auto Define = [this](StringRef S, DefinedRegular<ELFT> *&Sym1,
                       DefinedRegular<ELFT> *&Sym2) {
    Sym1 = Symtab<ELFT>::X->addIgnored(S, STV_DEFAULT);

    // The name without the underscore is not a reserved name,
    // so it is defined only when there is a reference against it.
    assert(S.startswith("_"));
    S = S.substr(1);
    if (SymbolBody *B = Symtab<ELFT>::X->find(S))
      if (B->isUndefined())
        Sym2 = Symtab<ELFT>::X->addAbsolute(S, STV_DEFAULT);
  };

  Define("_end", ElfSym<ELFT>::End, ElfSym<ELFT>::End2);
  Define("_etext", ElfSym<ELFT>::Etext, ElfSym<ELFT>::Etext2);
  Define("_edata", ElfSym<ELFT>::Edata, ElfSym<ELFT>::Edata2);
}

// Sort input sections by section name suffixes for
// __attribute__((init_priority(N))).
template <class ELFT> static void sortInitFini(OutputSectionBase<ELFT> *S) {
  if (S)
    reinterpret_cast<OutputSection<ELFT> *>(S)->sortInitFini();
}

// Sort input sections by the special rule for .ctors and .dtors.
template <class ELFT> static void sortCtorsDtors(OutputSectionBase<ELFT> *S) {
  if (S)
    reinterpret_cast<OutputSection<ELFT> *>(S)->sortCtorsDtors();
}

template <class ELFT>
void Writer<ELFT>::forEachRelSec(
    std::function<void(InputSectionBase<ELFT> &, const typename ELFT::Shdr &)>
        Fn) {
  for (elf::ObjectFile<ELFT> *F : Symtab<ELFT>::X->getObjectFiles()) {
    for (InputSectionBase<ELFT> *IS : F->getSections()) {
      if (isDiscarded(IS))
        continue;
      // Scan all relocations. Each relocation goes through a series
      // of tests to determine if it needs special treatment, such as
      // creating GOT, PLT, copy relocations, etc.
      // Note that relocations for non-alloc sections are directly
      // processed by InputSection::relocateNonAlloc.
      if (!(IS->Flags & SHF_ALLOC))
        continue;
      if (auto *S = dyn_cast<InputSection<ELFT>>(IS)) {
        for (const Elf_Shdr *RelSec : S->RelocSections)
          Fn(*S, *RelSec);
        continue;
      }
      if (auto *S = dyn_cast<EhInputSection<ELFT>>(IS))
        if (S->RelocSection)
          Fn(*S, *S->RelocSection);
    }
  }
}

template <class ELFT>
void Writer<ELFT>::addInputSec(InputSectionBase<ELFT> *IS) {
  if (isDiscarded(IS)) {
    reportDiscarded(IS);
    return;
  }
  OutputSectionBase<ELFT> *Sec;
  bool IsNew;
  StringRef OutsecName = getOutputSectionName(IS->Name);
  std::tie(Sec, IsNew) = Factory.create(IS, OutsecName);
  if (IsNew)
    OutputSections.push_back(Sec);
  Sec->addSection(IS);
}

template <class ELFT> void Writer<ELFT>::createSections() {
  for (elf::ObjectFile<ELFT> *F : Symtab<ELFT>::X->getObjectFiles())
    for (InputSectionBase<ELFT> *IS : F->getSections())
      addInputSec(IS);

  for (BinaryFile *F : Symtab<ELFT>::X->getBinaryFiles())
    for (InputSectionData *ID : F->getSections())
      addInputSec(cast<InputSection<ELFT>>(ID));

  sortInitFini(findSection(".init_array"));
  sortInitFini(findSection(".fini_array"));
  sortCtorsDtors(findSection(".ctors"));
  sortCtorsDtors(findSection(".dtors"));

  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    Sec->assignOffsets();
}

template <class ELFT> void Writer<ELFT>::sortSections() {
  if (!ScriptConfig->HasSections) {
    std::stable_sort(OutputSections.begin(), OutputSections.end(),
                     compareSectionsNonScript<ELFT>);
    return;
  }
  Script<ELFT>::X->adjustSectionsBeforeSorting();

  // The order of the sections in the script is arbitrary and may not agree with
  // compareSectionsNonScript. This means that we cannot easily define a
  // strict weak ordering. To see why, consider a comparison of a section in the
  // script and one not in the script. We have a two simple options:
  // * Make them equivalent (a is not less than b, and b is not less than a).
  //   The problem is then that equivalence has to be transitive and we can
  //   have sections a, b and c with only b in a script and a less than c
  //   which breaks this property.
  // * Use compareSectionsNonScript. Given that the script order doesn't have
  //   to match, we can end up with sections a, b, c, d where b and c are in the
  //   script and c is compareSectionsNonScript less than b. In which case d
  //   can be equivalent to c, a to b and d < a. As a concrete example:
  //   .a (rx) # not in script
  //   .b (rx) # in script
  //   .c (ro) # in script
  //   .d (ro) # not in script
  //
  // The way we define an order then is:
  // *  First put script sections at the start and sort the script and
  //    non-script sections independently.
  // *  Move each non-script section to the first position where it
  //    compareSectionsNonScript less than the successor.

  std::stable_sort(OutputSections.begin(), OutputSections.end(),
                   compareSections<ELFT>);

  auto I = OutputSections.begin();
  auto E = OutputSections.end();
  auto NonScriptI = std::find_if(I, E, [](OutputSectionBase<ELFT> *S) {
    return Script<ELFT>::X->getSectionIndex(S->getName()) == INT_MAX;
  });
  while (NonScriptI != E) {
    auto FirstGreater =
        std::find_if(I, NonScriptI, [&](OutputSectionBase<ELFT> *S) {
          return compareSectionsNonScript<ELFT>(*NonScriptI, S);
        });
    std::rotate(FirstGreater, NonScriptI, NonScriptI + 1);
    ++NonScriptI;
    ++I;
  }
}

// Create output section objects and add them to OutputSections.
template <class ELFT> void Writer<ELFT>::finalizeSections() {
  Out<ELFT>::DebugInfo = findSection(".debug_info");
  Out<ELFT>::PreinitArray = findSection(".preinit_array");
  Out<ELFT>::InitArray = findSection(".init_array");
  Out<ELFT>::FiniArray = findSection(".fini_array");

  // The linker needs to define SECNAME_start, SECNAME_end and SECNAME_stop
  // symbols for sections, so that the runtime can get the start and end
  // addresses of each section by section name. Add such symbols.
  if (!Config->Relocatable) {
    addStartEndSymbols();
    for (OutputSectionBase<ELFT> *Sec : OutputSections)
      addStartStopSymbols(Sec);
  }

  // Add _DYNAMIC symbol. Unlike GNU gold, our _DYNAMIC symbol has no type.
  // It should be okay as no one seems to care about the type.
  // Even the author of gold doesn't remember why gold behaves that way.
  // https://sourceware.org/ml/binutils/2002-03/msg00360.html
  if (Out<ELFT>::DynSymTab)
    Symtab<ELFT>::X->addSynthetic("_DYNAMIC", Out<ELFT>::Dynamic, 0,
                                  STV_HIDDEN);

  // Define __rel[a]_iplt_{start,end} symbols if needed.
  addRelIpltSymbols();

  if (!Out<ELFT>::EhFrame->empty()) {
    OutputSections.push_back(Out<ELFT>::EhFrame);
    Out<ELFT>::EhFrame->finalize();
  }

  // Scan relocations. This must be done after every symbol is declared so that
  // we can correctly decide if a dynamic relocation is needed.
  forEachRelSec(scanRelocations<ELFT>);

  // Now that we have defined all possible symbols including linker-
  // synthesized ones. Visit all symbols to give the finishing touches.
  for (Symbol *S : Symtab<ELFT>::X->getSymbols()) {
    SymbolBody *Body = S->body();

    if (!includeInSymtab<ELFT>(*Body))
      continue;
    if (Out<ELFT>::SymTab)
      Out<ELFT>::SymTab->addSymbol(Body);

    if (Out<ELFT>::DynSymTab && S->includeInDynsym()) {
      Out<ELFT>::DynSymTab->addSymbol(Body);
      if (auto *SS = dyn_cast<SharedSymbol<ELFT>>(Body))
        if (SS->file()->isNeeded())
          Out<ELFT>::VerNeed->addSymbol(SS);
    }
  }

  // Do not proceed if there was an undefined symbol.
  if (HasError)
    return;

  // If linker script processor hasn't added common symbol section yet,
  // then add it to .bss now.
  if (!InputSection<ELFT>::CommonInputSection->OutSec) {
    Out<ELFT>::Bss->addSection(InputSection<ELFT>::CommonInputSection);
    Out<ELFT>::Bss->assignOffsets();
  }

  // So far we have added sections from input object files.
  // This function adds linker-created Out<ELFT>::* sections.
  addPredefinedSections();

  // Adds linker generated input sections to
  // corresponding output sections.
  for (InputSection<ELFT> *S : In<ELFT>::Sections)
    addInputSec(S);

  sortSections();

  unsigned I = 1;
  for (OutputSectionBase<ELFT> *Sec : OutputSections) {
    Sec->SectionIndex = I++;
    Sec->setSHName(Out<ELFT>::ShStrTab->addString(Sec->getName()));
  }

  // Finalize linker generated sections.
  for (InputSection<ELFT> *S : In<ELFT>::Sections)
    if (S && S->OutSec)
      S->OutSec->assignOffsets();

  // Finalizers fix each section's size.
  // .dynsym is finalized early since that may fill up .gnu.hash.
  if (Out<ELFT>::DynSymTab)
    Out<ELFT>::DynSymTab->finalize();

  // Fill other section headers. The dynamic table is finalized
  // at the end because some tags like RELSZ depend on result
  // of finalizing other sections. The dynamic string table is
  // finalized once the .dynamic finalizer has added a few last
  // strings. See DynamicSection::finalize()
  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    if (Sec != Out<ELFT>::DynStrTab && Sec != Out<ELFT>::Dynamic)
      Sec->finalize();

  if (Out<ELFT>::DynSymTab)
    Out<ELFT>::Dynamic->finalize();

  // Now that all output offsets are fixed. Finalize mergeable sections
  // to fix their maps from input offsets to output offsets.
  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    Sec->finalizePieces();
}

template <class ELFT> bool Writer<ELFT>::needsGot() {
  if (!Out<ELFT>::Got->empty())
    return true;

  // We add the .got section to the result for dynamic MIPS target because
  // its address and properties are mentioned in the .dynamic section.
  if (Config->EMachine == EM_MIPS && !Config->Relocatable)
    return true;

  // If we have a relocation that is relative to GOT (such as GOTOFFREL),
  // we need to emit a GOT even if it's empty.
  return Out<ELFT>::Got->HasGotOffRel;
}

// This function add Out<ELFT>::* sections to OutputSections.
template <class ELFT> void Writer<ELFT>::addPredefinedSections() {
  auto Add = [&](OutputSectionBase<ELFT> *OS) {
    if (OS)
      OutputSections.push_back(OS);
  };

  // Add .interp at first because some loaders want to see that section
  // on the first page of the executable file when loaded into memory.
  if (Out<ELFT>::Interp)
    OutputSections.insert(OutputSections.begin(), Out<ELFT>::Interp);

  // This order is not the same as the final output order
  // because we sort the sections using their attributes below.
  if (Out<ELFT>::GdbIndex && Out<ELFT>::DebugInfo)
    Add(Out<ELFT>::GdbIndex);
  Add(Out<ELFT>::SymTab);
  Add(Out<ELFT>::ShStrTab);
  Add(Out<ELFT>::StrTab);
  if (Out<ELFT>::DynSymTab) {
    Add(Out<ELFT>::DynSymTab);

    bool HasVerNeed = Out<ELFT>::VerNeed->getNeedNum() != 0;
    if (Out<ELFT>::VerDef || HasVerNeed)
      Add(Out<ELFT>::VerSym);
    Add(Out<ELFT>::VerDef);
    if (HasVerNeed)
      Add(Out<ELFT>::VerNeed);

    Add(Out<ELFT>::GnuHashTab);
    Add(Out<ELFT>::HashTab);
    Add(Out<ELFT>::Dynamic);
    Add(Out<ELFT>::DynStrTab);
    if (Out<ELFT>::RelaDyn->hasRelocs())
      Add(Out<ELFT>::RelaDyn);
    Add(Out<ELFT>::MipsRldMap);
  }

  // We always need to add rel[a].plt to output if it has entries.
  // Even during static linking it can contain R_[*]_IRELATIVE relocations.
  if (Out<ELFT>::RelaPlt && Out<ELFT>::RelaPlt->hasRelocs())
    Add(Out<ELFT>::RelaPlt);

  if (needsGot())
    Add(Out<ELFT>::Got);
  if (Out<ELFT>::GotPlt && !Out<ELFT>::GotPlt->empty())
    Add(Out<ELFT>::GotPlt);
  if (!Out<ELFT>::Plt->empty())
    Add(Out<ELFT>::Plt);
  if (!Out<ELFT>::EhFrame->empty())
    Add(Out<ELFT>::EhFrameHdr);
  if (Out<ELFT>::Bss->getSize() > 0)
    Add(Out<ELFT>::Bss);
}

// The linker is expected to define SECNAME_start and SECNAME_end
// symbols for a few sections. This function defines them.
template <class ELFT> void Writer<ELFT>::addStartEndSymbols() {
  auto Define = [&](StringRef Start, StringRef End,
                    OutputSectionBase<ELFT> *OS) {
    // These symbols resolve to the image base if the section does not exist.
    addOptionalSynthetic(Start, OS, 0);
    addOptionalSynthetic(End, OS, OS ? DefinedSynthetic<ELFT>::SectionEnd : 0);
  };

  Define("__preinit_array_start", "__preinit_array_end",
         Out<ELFT>::PreinitArray);
  Define("__init_array_start", "__init_array_end", Out<ELFT>::InitArray);
  Define("__fini_array_start", "__fini_array_end", Out<ELFT>::FiniArray);

  if (OutputSectionBase<ELFT> *Sec = findSection(".ARM.exidx"))
    Define("__exidx_start", "__exidx_end", Sec);
}

// If a section name is valid as a C identifier (which is rare because of
// the leading '.'), linkers are expected to define __start_<secname> and
// __stop_<secname> symbols. They are at beginning and end of the section,
// respectively. This is not requested by the ELF standard, but GNU ld and
// gold provide the feature, and used by many programs.
template <class ELFT>
void Writer<ELFT>::addStartStopSymbols(OutputSectionBase<ELFT> *Sec) {
  StringRef S = Sec->getName();
  if (!isValidCIdentifier(S))
    return;
  addOptionalSynthetic(Saver.save("__start_" + S), Sec, 0, STV_DEFAULT);
  addOptionalSynthetic(Saver.save("__stop_" + S), Sec,
                       DefinedSynthetic<ELFT>::SectionEnd, STV_DEFAULT);
}

template <class ELFT>
OutputSectionBase<ELFT> *Writer<ELFT>::findSection(StringRef Name) {
  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    if (Sec->getName() == Name)
      return Sec;
  return nullptr;
}

template <class ELFT> static bool needsPtLoad(OutputSectionBase<ELFT> *Sec) {
  if (!(Sec->getFlags() & SHF_ALLOC))
    return false;

  // Don't allocate VA space for TLS NOBITS sections. The PT_TLS PHDR is
  // responsible for allocating space for them, not the PT_LOAD that
  // contains the TLS initialization image.
  if (Sec->getFlags() & SHF_TLS && Sec->getType() == SHT_NOBITS)
    return false;
  return true;
}

// Linker scripts are responsible for aligning addresses. Unfortunately, most
// linker scripts are designed for creating two PT_LOADs only, one RX and one
// RW. This means that there is no alignment in the RO to RX transition and we
// cannot create a PT_LOAD there.
template <class ELFT>
static typename ELFT::uint computeFlags(typename ELFT::uint F) {
  if (ScriptConfig->HasSections && !(F & PF_W))
    return F | PF_X;
  return F;
}

// Decide which program headers to create and which sections to include in each
// one.
template <class ELFT> std::vector<PhdrEntry<ELFT>> Writer<ELFT>::createPhdrs() {
  std::vector<Phdr> Ret;
  auto AddHdr = [&](unsigned Type, unsigned Flags) -> Phdr * {
    Ret.emplace_back(Type, Flags);
    return &Ret.back();
  };

  // The first phdr entry is PT_PHDR which describes the program header itself.
  Phdr &Hdr = *AddHdr(PT_PHDR, PF_R);
  Hdr.add(Out<ELFT>::ProgramHeaders);

  // PT_INTERP must be the second entry if exists.
  if (Out<ELFT>::Interp) {
    Phdr &Hdr = *AddHdr(PT_INTERP, Out<ELFT>::Interp->getPhdrFlags());
    Hdr.add(Out<ELFT>::Interp);
  }

  // Add the first PT_LOAD segment for regular output sections.
  uintX_t Flags = computeFlags<ELFT>(PF_R);
  Phdr *Load = AddHdr(PT_LOAD, Flags);
  if (!ScriptConfig->HasSections) {
    Load->add(Out<ELFT>::ElfHeader);
    Load->add(Out<ELFT>::ProgramHeaders);
  }

  Phdr TlsHdr(PT_TLS, PF_R);
  Phdr RelRo(PT_GNU_RELRO, PF_R);
  Phdr Note(PT_NOTE, PF_R);
  Phdr ARMExidx(PT_ARM_EXIDX, PF_R);
  for (OutputSectionBase<ELFT> *Sec : OutputSections) {
    if (!(Sec->getFlags() & SHF_ALLOC))
      break;

    // If we meet TLS section then we create TLS header
    // and put all TLS sections inside for further use when
    // assign addresses.
    if (Sec->getFlags() & SHF_TLS)
      TlsHdr.add(Sec);

    if (!needsPtLoad(Sec))
      continue;

    // Segments are contiguous memory regions that has the same attributes
    // (e.g. executable or writable). There is one phdr for each segment.
    // Therefore, we need to create a new phdr when the next section has
    // different flags or is loaded at a discontiguous address using AT linker
    // script command.
    uintX_t NewFlags = computeFlags<ELFT>(Sec->getPhdrFlags());
    if (Script<ELFT>::X->hasLMA(Sec->getName()) || Flags != NewFlags) {
      Load = AddHdr(PT_LOAD, NewFlags);
      Flags = NewFlags;
    }

    Load->add(Sec);

    if (isRelroSection(Sec))
      RelRo.add(Sec);
    if (Sec->getType() == SHT_NOTE)
      Note.add(Sec);
    if (Config->EMachine == EM_ARM && Sec->getType() == SHT_ARM_EXIDX)
      ARMExidx.add(Sec);
  }

  // Add the TLS segment unless it's empty.
  if (TlsHdr.First)
    Ret.push_back(std::move(TlsHdr));

  // Add an entry for .dynamic.
  if (Out<ELFT>::DynSymTab) {
    Phdr &H = *AddHdr(PT_DYNAMIC, Out<ELFT>::Dynamic->getPhdrFlags());
    H.add(Out<ELFT>::Dynamic);
  }

  // PT_GNU_RELRO includes all sections that should be marked as
  // read-only by dynamic linker after proccessing relocations.
  if (RelRo.First)
    Ret.push_back(std::move(RelRo));

  // PT_GNU_EH_FRAME is a special section pointing on .eh_frame_hdr.
  if (!Out<ELFT>::EhFrame->empty() && Out<ELFT>::EhFrameHdr) {
    Phdr &Hdr = *AddHdr(PT_GNU_EH_FRAME, Out<ELFT>::EhFrameHdr->getPhdrFlags());
    Hdr.add(Out<ELFT>::EhFrameHdr);
  }

  // PT_OPENBSD_RANDOMIZE specifies the location and size of a part of the
  // memory image of the program that must be filled with random data before any
  // code in the object is executed.
  if (OutputSectionBase<ELFT> *Sec = findSection(".openbsd.randomdata")) {
    Phdr &Hdr = *AddHdr(PT_OPENBSD_RANDOMIZE, Sec->getPhdrFlags());
    Hdr.add(Sec);
  }

  // PT_ARM_EXIDX is the ARM EHABI equivalent of PT_GNU_EH_FRAME
  if (ARMExidx.First)
    Ret.push_back(std::move(ARMExidx));

  // PT_GNU_STACK is a special section to tell the loader to make the
  // pages for the stack non-executable.
  if (!Config->ZExecstack) {
    Phdr &Hdr = *AddHdr(PT_GNU_STACK, PF_R | PF_W);
    if (Config->ZStackSize != uint64_t(-1))
      Hdr.H.p_memsz = Config->ZStackSize;
  }

  // PT_OPENBSD_WXNEEDED is a OpenBSD-specific header to mark the executable
  // is expected to perform W^X violations, such as calling mprotect(2) or
  // mmap(2) with PROT_WRITE | PROT_EXEC, which is prohibited by default on
  // OpenBSD.
  if (Config->ZWxneeded)
    AddHdr(PT_OPENBSD_WXNEEDED, PF_X);

  if (Note.First)
    Ret.push_back(std::move(Note));
  return Ret;
}

// The first section of each PT_LOAD and the first section after PT_GNU_RELRO
// have to be page aligned so that the dynamic linker can set the permissions.
template <class ELFT> void Writer<ELFT>::fixSectionAlignments() {
  for (const Phdr &P : Phdrs)
    if (P.H.p_type == PT_LOAD)
      P.First->PageAlign = true;

  for (const Phdr &P : Phdrs) {
    if (P.H.p_type != PT_GNU_RELRO)
      continue;
    // Find the first section after PT_GNU_RELRO. If it is in a PT_LOAD we
    // have to align it to a page.
    auto End = OutputSections.end();
    auto I = std::find(OutputSections.begin(), End, P.Last);
    if (I == End || (I + 1) == End)
      continue;
    OutputSectionBase<ELFT> *Sec = *(I + 1);
    if (needsPtLoad(Sec))
      Sec->PageAlign = true;
  }
}

// We should set file offsets and VAs for elf header and program headers
// sections. These are special, we do not include them into output sections
// list, but have them to simplify the code.
template <class ELFT> void Writer<ELFT>::fixHeaders() {
  uintX_t BaseVA = ScriptConfig->HasSections ? 0 : Config->ImageBase;
  Out<ELFT>::ElfHeader->setVA(BaseVA);
  uintX_t Off = Out<ELFT>::ElfHeader->getSize();
  Out<ELFT>::ProgramHeaders->setVA(Off + BaseVA);
  Out<ELFT>::ProgramHeaders->setSize(sizeof(Elf_Phdr) * Phdrs.size());
}

// Assign VAs (addresses at run-time) to output sections.
template <class ELFT> void Writer<ELFT>::assignAddresses() {
  uintX_t VA = Config->ImageBase + getHeaderSize<ELFT>();
  uintX_t ThreadBssOffset = 0;
  for (OutputSectionBase<ELFT> *Sec : OutputSections) {
    uintX_t Alignment = Sec->getAlignment();
    if (Sec->PageAlign)
      Alignment = std::max<uintX_t>(Alignment, Config->MaxPageSize);

    auto I = Config->SectionStartMap.find(Sec->getName());
    if (I != Config->SectionStartMap.end())
      VA = I->second;

    // We only assign VAs to allocated sections.
    if (needsPtLoad(Sec)) {
      VA = alignTo(VA, Alignment);
      Sec->setVA(VA);
      VA += Sec->getSize();
    } else if (Sec->getFlags() & SHF_TLS && Sec->getType() == SHT_NOBITS) {
      uintX_t TVA = VA + ThreadBssOffset;
      TVA = alignTo(TVA, Alignment);
      Sec->setVA(TVA);
      ThreadBssOffset = TVA - VA + Sec->getSize();
    }
  }
}

// Adjusts the file alignment for a given output section and returns
// its new file offset. The file offset must be the same with its
// virtual address (modulo the page size) so that the loader can load
// executables without any address adjustment.
template <class ELFT, class uintX_t>
static uintX_t getFileAlignment(uintX_t Off, OutputSectionBase<ELFT> *Sec) {
  uintX_t Alignment = Sec->getAlignment();
  if (Sec->PageAlign)
    Alignment = std::max<uintX_t>(Alignment, Config->MaxPageSize);
  Off = alignTo(Off, Alignment);

  OutputSectionBase<ELFT> *First = Sec->FirstInPtLoad;
  // If the section is not in a PT_LOAD, we have no other constraint.
  if (!First)
    return Off;

  // If two sections share the same PT_LOAD the file offset is calculated using
  // this formula: Off2 = Off1 + (VA2 - VA1).
  if (Sec == First)
    return alignTo(Off, Target->MaxPageSize, Sec->getVA());
  return First->getFileOffset() + Sec->getVA() - First->getVA();
}

template <class ELFT, class uintX_t>
void setOffset(OutputSectionBase<ELFT> *Sec, uintX_t &Off) {
  if (Sec->getType() == SHT_NOBITS) {
    Sec->setFileOffset(Off);
    return;
  }

  Off = getFileAlignment<ELFT>(Off, Sec);
  Sec->setFileOffset(Off);
  Off += Sec->getSize();
}

template <class ELFT> void Writer<ELFT>::assignFileOffsetsBinary() {
  uintX_t Off = 0;
  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    if (Sec->getFlags() & SHF_ALLOC)
      setOffset(Sec, Off);
  FileSize = alignTo(Off, sizeof(uintX_t));
}

// Assign file offsets to output sections.
template <class ELFT> void Writer<ELFT>::assignFileOffsets() {
  uintX_t Off = 0;
  setOffset(Out<ELFT>::ElfHeader, Off);
  setOffset(Out<ELFT>::ProgramHeaders, Off);

  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    setOffset(Sec, Off);

  SectionHeaderOff = alignTo(Off, sizeof(uintX_t));
  FileSize = SectionHeaderOff + (OutputSections.size() + 1) * sizeof(Elf_Shdr);
}

// Finalize the program headers. We call this function after we assign
// file offsets and VAs to all sections.
template <class ELFT> void Writer<ELFT>::setPhdrs() {
  for (Phdr &P : Phdrs) {
    Elf_Phdr &H = P.H;
    OutputSectionBase<ELFT> *First = P.First;
    OutputSectionBase<ELFT> *Last = P.Last;
    if (First) {
      H.p_filesz = Last->getFileOff() - First->getFileOff();
      if (Last->getType() != SHT_NOBITS)
        H.p_filesz += Last->getSize();
      H.p_memsz = Last->getVA() + Last->getSize() - First->getVA();
      H.p_offset = First->getFileOff();
      H.p_vaddr = First->getVA();
      if (!P.HasLMA)
        H.p_paddr = First->getLMA();
    }
    if (H.p_type == PT_LOAD)
      H.p_align = Config->MaxPageSize;
    else if (H.p_type == PT_GNU_RELRO)
      H.p_align = 1;

    // The TLS pointer goes after PT_TLS. At least glibc will align it,
    // so round up the size to make sure the offsets are correct.
    if (H.p_type == PT_TLS) {
      Out<ELFT>::TlsPhdr = &H;
      if (H.p_memsz)
        H.p_memsz = alignTo(H.p_memsz, H.p_align);
    }
  }
}

template <class ELFT> static typename ELFT::uint getEntryAddr() {
  if (Config->Entry.empty())
    return Config->EntryAddr;
  if (SymbolBody *B = Symtab<ELFT>::X->find(Config->Entry))
    return B->getVA<ELFT>();
  warn("entry symbol " + Config->Entry + " not found, assuming 0");
  return 0;
}

template <class ELFT> static uint8_t getELFEncoding() {
  if (ELFT::TargetEndianness == llvm::support::little)
    return ELFDATA2LSB;
  return ELFDATA2MSB;
}

static uint16_t getELFType() {
  if (Config->Pic)
    return ET_DYN;
  if (Config->Relocatable)
    return ET_REL;
  return ET_EXEC;
}

// This function is called after we have assigned address and size
// to each section. This function fixes some predefined absolute
// symbol values that depend on section address and size.
template <class ELFT> void Writer<ELFT>::fixAbsoluteSymbols() {
  // __ehdr_start is the location of program headers.
  if (ElfSym<ELFT>::EhdrStart)
    ElfSym<ELFT>::EhdrStart->Value = Out<ELFT>::ProgramHeaders->getVA();

  auto Set = [](DefinedRegular<ELFT> *S1, DefinedRegular<ELFT> *S2, uintX_t V) {
    if (S1)
      S1->Value = V;
    if (S2)
      S2->Value = V;
  };

  // _etext is the first location after the last read-only loadable segment.
  // _edata is the first location after the last read-write loadable segment.
  // _end is the first location after the uninitialized data region.
  for (Phdr &P : Phdrs) {
    Elf_Phdr &H = P.H;
    if (H.p_type != PT_LOAD)
      continue;
    Set(ElfSym<ELFT>::End, ElfSym<ELFT>::End2, H.p_vaddr + H.p_memsz);

    uintX_t Val = H.p_vaddr + H.p_filesz;
    if (H.p_flags & PF_W)
      Set(ElfSym<ELFT>::Edata, ElfSym<ELFT>::Edata2, Val);
    else
      Set(ElfSym<ELFT>::Etext, ElfSym<ELFT>::Etext2, Val);
  }
}

template <class ELFT> void Writer<ELFT>::writeHeader() {
  uint8_t *Buf = Buffer->getBufferStart();
  memcpy(Buf, "\177ELF", 4);

  // Write the ELF header.
  auto *EHdr = reinterpret_cast<Elf_Ehdr *>(Buf);
  EHdr->e_ident[EI_CLASS] = ELFT::Is64Bits ? ELFCLASS64 : ELFCLASS32;
  EHdr->e_ident[EI_DATA] = getELFEncoding<ELFT>();
  EHdr->e_ident[EI_VERSION] = EV_CURRENT;
  EHdr->e_ident[EI_OSABI] = Config->OSABI;
  EHdr->e_type = getELFType();
  EHdr->e_machine = Config->EMachine;
  EHdr->e_version = EV_CURRENT;
  EHdr->e_entry = getEntryAddr<ELFT>();
  EHdr->e_shoff = SectionHeaderOff;
  EHdr->e_ehsize = sizeof(Elf_Ehdr);
  EHdr->e_phnum = Phdrs.size();
  EHdr->e_shentsize = sizeof(Elf_Shdr);
  EHdr->e_shnum = OutputSections.size() + 1;
  EHdr->e_shstrndx = Out<ELFT>::ShStrTab->SectionIndex;

  if (Config->EMachine == EM_ARM)
    // We don't currently use any features incompatible with EF_ARM_EABI_VER5,
    // but we don't have any firm guarantees of conformance. Linux AArch64
    // kernels (as of 2016) require an EABI version to be set.
    EHdr->e_flags = EF_ARM_EABI_VER5;
  else if (Config->EMachine == EM_MIPS)
    EHdr->e_flags = getMipsEFlags<ELFT>();

  if (!Config->Relocatable) {
    EHdr->e_phoff = sizeof(Elf_Ehdr);
    EHdr->e_phentsize = sizeof(Elf_Phdr);
  }

  // Write the program header table.
  auto *HBuf = reinterpret_cast<Elf_Phdr *>(Buf + EHdr->e_phoff);
  for (Phdr &P : Phdrs)
    *HBuf++ = P.H;

  // Write the section header table. Note that the first table entry is null.
  auto *SHdrs = reinterpret_cast<Elf_Shdr *>(Buf + EHdr->e_shoff);
  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    Sec->writeHeaderTo(++SHdrs);
}

template <class ELFT> void Writer<ELFT>::openFile() {
  ErrorOr<std::unique_ptr<FileOutputBuffer>> BufferOrErr =
      FileOutputBuffer::create(Config->OutputFile, FileSize,
                               FileOutputBuffer::F_executable);
  if (auto EC = BufferOrErr.getError())
    error(EC, "failed to open " + Config->OutputFile);
  else
    Buffer = std::move(*BufferOrErr);
}

template <class ELFT> void Writer<ELFT>::writeSectionsBinary() {
  uint8_t *Buf = Buffer->getBufferStart();
  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    if (Sec->getFlags() & SHF_ALLOC)
      Sec->writeTo(Buf + Sec->getFileOff());
}

// Convert the .ARM.exidx table entries that use relative PREL31 offsets to
// Absolute addresses. This form is internal to LLD and is only used to
// make reordering the table simpler.
static void ARMExidxEntryPrelToAbs(uint8_t *Loc, uint64_t EntryVA) {
  uint64_t Addr = Target->getImplicitAddend(Loc, R_ARM_PREL31) + EntryVA;
  bool InlineEntry =
      (read32le(Loc + 4) == 1 || (read32le(Loc + 4) & 0x80000000));
  if (InlineEntry)
    // Set flag in unused bit of code address so that when we convert back we
    // know which table entries to leave alone.
    Addr |= 0x1;
  else
    write32le(Loc + 4,
              Target->getImplicitAddend(Loc + 4, R_ARM_PREL31) + EntryVA + 4);
  write32le(Loc, Addr);
}

// Convert the .ARM.exidx table entries from the internal to LLD form using
// absolute addresses back to relative PREL31 offsets.
static void ARMExidxEntryAbsToPrel(uint8_t *Loc, uint64_t EntryVA) {
  uint64_t Off = read32le(Loc) - EntryVA;
  // ARMExidxEntryPreltoAbs sets bit 0 if the table entry has inline data
  // that is not an address
  bool InlineEntry = Off & 0x1;
  Target->relocateOne(Loc, R_ARM_PREL31, Off & ~0x1);
  if (!InlineEntry)
    Target->relocateOne(Loc + 4, R_ARM_PREL31,
                        read32le(Loc + 4) - (EntryVA + 4));
}

// The table formed by the .ARM.exidx OutputSection has entries with two
// 4-byte fields:
// | PREL31 offset to function | Action to take for function |
// The table must be ordered in ascending virtual address of the functions
// identified by the first field of the table. Instead of using the
// SHF_LINK_ORDER dependency to reorder the sections prior to relocation we
// sort the table post-relocation.
// Ref: Exception handling ABI for the ARM architecture
static void sortARMExidx(uint8_t *Buf, uint64_t OutSecVA, uint64_t Size) {
  struct ARMExidxEntry {
    ulittle32_t Target;
    ulittle32_t Action;
  };
  ARMExidxEntry *Start = (ARMExidxEntry *)Buf;
  size_t NumEnt = Size / sizeof(ARMExidxEntry);
  for (uint64_t Off = 0; Off < Size; Off += 8)
    ARMExidxEntryPrelToAbs(Buf + Off, OutSecVA + Off);
  std::stable_sort(Start, Start + NumEnt,
                   [](const ARMExidxEntry &A, const ARMExidxEntry &B) {
                     return A.Target < B.Target;
                   });
  for (uint64_t Off = 0; Off < Size; Off += 8)
    ARMExidxEntryAbsToPrel(Buf + Off, OutSecVA + Off);
}

// Write section contents to a mmap'ed file.
template <class ELFT> void Writer<ELFT>::writeSections() {
  uint8_t *Buf = Buffer->getBufferStart();

  // PPC64 needs to process relocations in the .opd section
  // before processing relocations in code-containing sections.
  Out<ELFT>::Opd = findSection(".opd");
  if (Out<ELFT>::Opd) {
    Out<ELFT>::OpdBuf = Buf + Out<ELFT>::Opd->getFileOff();
    Out<ELFT>::Opd->writeTo(Buf + Out<ELFT>::Opd->getFileOff());
  }

  for (OutputSectionBase<ELFT> *Sec : OutputSections)
    if (Sec != Out<ELFT>::Opd && Sec != Out<ELFT>::EhFrameHdr)
      Sec->writeTo(Buf + Sec->getFileOff());

  OutputSectionBase<ELFT> *ARMExidx = findSection(".ARM.exidx");
  if (!Config->Relocatable)
    if (auto *OS = dyn_cast_or_null<OutputSection<ELFT>>(ARMExidx))
      sortARMExidx(Buf + OS->getFileOff(), OS->getVA(), OS->getSize());

  // The .eh_frame_hdr depends on .eh_frame section contents, therefore
  // it should be written after .eh_frame is written.
  if (!Out<ELFT>::EhFrame->empty() && Out<ELFT>::EhFrameHdr)
    Out<ELFT>::EhFrameHdr->writeTo(Buf + Out<ELFT>::EhFrameHdr->getFileOff());
}

template <class ELFT> void Writer<ELFT>::writeBuildId() {
  if (!In<ELFT>::BuildId || !In<ELFT>::BuildId->OutSec)
    return;

  // Compute a hash of all sections of the output file.
  uint8_t *Start = Buffer->getBufferStart();
  uint8_t *End = Start + FileSize;
  In<ELFT>::BuildId->writeBuildId({Start, End});
}

template void elf::writeResult<ELF32LE>();
template void elf::writeResult<ELF32BE>();
template void elf::writeResult<ELF64LE>();
template void elf::writeResult<ELF64BE>();

template struct elf::PhdrEntry<ELF32LE>;
template struct elf::PhdrEntry<ELF32BE>;
template struct elf::PhdrEntry<ELF64LE>;
template struct elf::PhdrEntry<ELF64BE>;

template bool elf::isRelroSection<ELF32LE>(OutputSectionBase<ELF32LE> *);
template bool elf::isRelroSection<ELF32BE>(OutputSectionBase<ELF32BE> *);
template bool elf::isRelroSection<ELF64LE>(OutputSectionBase<ELF64LE> *);
template bool elf::isRelroSection<ELF64BE>(OutputSectionBase<ELF64BE> *);

template void elf::reportDiscarded<ELF32LE>(InputSectionBase<ELF32LE> *);
template void elf::reportDiscarded<ELF32BE>(InputSectionBase<ELF32BE> *);
template void elf::reportDiscarded<ELF64LE>(InputSectionBase<ELF64LE> *);
template void elf::reportDiscarded<ELF64BE>(InputSectionBase<ELF64BE> *);
