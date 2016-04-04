//===- InputFiles.cpp -----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Error.h"
#include "InputSection.h"
#include "Symbols.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/IRObjectFile.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace llvm::sys::fs;

using namespace lld;
using namespace lld::elf;

template <class ELFT>
static ELFFile<ELFT> createELFObj(MemoryBufferRef MB) {
  std::error_code EC;
  ELFFile<ELFT> F(MB.getBuffer(), EC);
  check(EC);
  return F;
}

template <class ELFT>
ELFFileBase<ELFT>::ELFFileBase(Kind K, MemoryBufferRef MB)
    : InputFile(K, MB), ELFObj(createELFObj<ELFT>(MB)) {}

template <class ELFT>
ELFKind ELFFileBase<ELFT>::getELFKind() {
  if (ELFT::TargetEndianness == support::little)
    return ELFT::Is64Bits ? ELF64LEKind : ELF32LEKind;
  return ELFT::Is64Bits ? ELF64BEKind : ELF32BEKind;
}

template <class ELFT>
typename ELFT::SymRange ELFFileBase<ELFT>::getElfSymbols(bool OnlyGlobals) {
  if (!Symtab)
    return Elf_Sym_Range(nullptr, nullptr);
  Elf_Sym_Range Syms = ELFObj.symbols(Symtab);
  uint32_t NumSymbols = std::distance(Syms.begin(), Syms.end());
  uint32_t FirstNonLocal = Symtab->sh_info;
  if (FirstNonLocal > NumSymbols)
    fatal("invalid sh_info in symbol table");

  if (OnlyGlobals)
    return make_range(Syms.begin() + FirstNonLocal, Syms.end());
  return make_range(Syms.begin(), Syms.end());
}

template <class ELFT>
uint32_t ELFFileBase<ELFT>::getSectionIndex(const Elf_Sym &Sym) const {
  uint32_t I = Sym.st_shndx;
  if (I == ELF::SHN_XINDEX)
    return ELFObj.getExtendedSymbolTableIndex(&Sym, Symtab, SymtabSHNDX);
  if (I >= ELF::SHN_LORESERVE)
    return 0;
  return I;
}

template <class ELFT> void ELFFileBase<ELFT>::initStringTable() {
  if (!Symtab)
    return;
  StringTable = check(ELFObj.getStringTableForSymtab(*Symtab));
}

template <class ELFT>
elf::ObjectFile<ELFT>::ObjectFile(MemoryBufferRef M)
    : ELFFileBase<ELFT>(Base::ObjectKind, M) {}

template <class ELFT>
ArrayRef<SymbolBody *> elf::ObjectFile<ELFT>::getNonLocalSymbols() {
  if (!this->Symtab)
    return this->SymbolBodies;
  uint32_t FirstNonLocal = this->Symtab->sh_info;
  return makeArrayRef(this->SymbolBodies).slice(FirstNonLocal);
}

template <class ELFT>
ArrayRef<SymbolBody *> elf::ObjectFile<ELFT>::getLocalSymbols() {
  if (!this->Symtab)
    return this->SymbolBodies;
  uint32_t FirstNonLocal = this->Symtab->sh_info;
  return makeArrayRef(this->SymbolBodies).slice(1, FirstNonLocal - 1);
}

template <class ELFT>
ArrayRef<SymbolBody *> elf::ObjectFile<ELFT>::getSymbols() {
  if (!this->Symtab)
    return this->SymbolBodies;
  return makeArrayRef(this->SymbolBodies).slice(1);
}

template <class ELFT> uint32_t elf::ObjectFile<ELFT>::getMipsGp0() const {
  if (MipsReginfo)
    return MipsReginfo->Reginfo->ri_gp_value;
  return 0;
}

template <class ELFT>
void elf::ObjectFile<ELFT>::parse(DenseSet<StringRef> &ComdatGroups) {
  // Read section and symbol tables.
  initializeSections(ComdatGroups);
  initializeSymbols();
}

// Sections with SHT_GROUP and comdat bits define comdat section groups.
// They are identified and deduplicated by group name. This function
// returns a group name.
template <class ELFT>
StringRef elf::ObjectFile<ELFT>::getShtGroupSignature(const Elf_Shdr &Sec) {
  const ELFFile<ELFT> &Obj = this->ELFObj;
  uint32_t SymtabdSectionIndex = Sec.sh_link;
  const Elf_Shdr *SymtabSec = check(Obj.getSection(SymtabdSectionIndex));
  uint32_t SymIndex = Sec.sh_info;
  const Elf_Sym *Sym = Obj.getSymbol(SymtabSec, SymIndex);
  StringRef StringTable = check(Obj.getStringTableForSymtab(*SymtabSec));
  return check(Sym->getName(StringTable));
}

template <class ELFT>
ArrayRef<typename elf::ObjectFile<ELFT>::Elf_Word>
elf::ObjectFile<ELFT>::getShtGroupEntries(const Elf_Shdr &Sec) {
  const ELFFile<ELFT> &Obj = this->ELFObj;
  ArrayRef<Elf_Word> Entries =
      check(Obj.template getSectionContentsAsArray<Elf_Word>(&Sec));
  if (Entries.empty() || Entries[0] != GRP_COMDAT)
    fatal("unsupported SHT_GROUP format");
  return Entries.slice(1);
}

template <class ELFT> static bool shouldMerge(const typename ELFT::Shdr &Sec) {
  typedef typename ELFT::uint uintX_t;
  uintX_t Flags = Sec.sh_flags;
  if (!(Flags & SHF_MERGE))
    return false;
  if (Flags & SHF_WRITE)
    fatal("writable SHF_MERGE sections are not supported");
  uintX_t EntSize = Sec.sh_entsize;
  if (!EntSize || Sec.sh_size % EntSize)
    fatal("SHF_MERGE section size must be a multiple of sh_entsize");

  // Don't try to merge if the aligment is larger than the sh_entsize and this
  // is not SHF_STRINGS.
  //
  // Since this is not a SHF_STRINGS, we would need to pad after every entity.
  // It would be equivalent for the producer of the .o to just set a larger
  // sh_entsize.
  if (Flags & SHF_STRINGS)
    return true;

  if (Sec.sh_addralign > EntSize)
    return false;

  return true;
}

template <class ELFT>
void elf::ObjectFile<ELFT>::initializeSections(
    DenseSet<StringRef> &ComdatGroups) {
  uint64_t Size = this->ELFObj.getNumSections();
  Sections.resize(Size);
  unsigned I = -1;
  const ELFFile<ELFT> &Obj = this->ELFObj;
  for (const Elf_Shdr &Sec : Obj.sections()) {
    ++I;
    if (Sections[I] == &InputSection<ELFT>::Discarded)
      continue;

    switch (Sec.sh_type) {
    case SHT_GROUP:
      Sections[I] = &InputSection<ELFT>::Discarded;
      if (ComdatGroups.insert(getShtGroupSignature(Sec)).second)
        continue;
      for (uint32_t SecIndex : getShtGroupEntries(Sec)) {
        if (SecIndex >= Size)
          fatal("invalid section index in group");
        Sections[SecIndex] = &InputSection<ELFT>::Discarded;
      }
      break;
    case SHT_SYMTAB:
      this->Symtab = &Sec;
      break;
    case SHT_SYMTAB_SHNDX:
      this->SymtabSHNDX = check(Obj.getSHNDXTable(Sec));
      break;
    case SHT_STRTAB:
    case SHT_NULL:
      break;
    case SHT_RELA:
    case SHT_REL: {
      // This section contains relocation information.
      // If -r is given, we do not interpret or apply relocation
      // but just copy relocation sections to output.
      if (Config->Relocatable) {
        Sections[I] = new (Alloc) InputSection<ELFT>(this, &Sec);
        break;
      }

      // Find the relocation target section and associate this
      // section with it.
      InputSectionBase<ELFT> *Target = getRelocTarget(Sec);
      if (!Target)
        break;
      if (auto *S = dyn_cast<InputSection<ELFT>>(Target)) {
        S->RelocSections.push_back(&Sec);
        break;
      }
      if (auto *S = dyn_cast<EHInputSection<ELFT>>(Target)) {
        if (S->RelocSection)
          fatal("multiple relocation sections to .eh_frame are not supported");
        S->RelocSection = &Sec;
        break;
      }
      fatal("relocations pointing to SHF_MERGE are not supported");
    }
    default:
      Sections[I] = createInputSection(Sec);
    }
  }
}

template <class ELFT>
InputSectionBase<ELFT> *
elf::ObjectFile<ELFT>::getRelocTarget(const Elf_Shdr &Sec) {
  uint32_t Idx = Sec.sh_info;
  if (Idx >= Sections.size())
    fatal("invalid relocated section index");
  InputSectionBase<ELFT> *Target = Sections[Idx];

  // Strictly speaking, a relocation section must be included in the
  // group of the section it relocates. However, LLVM 3.3 and earlier
  // would fail to do so, so we gracefully handle that case.
  if (Target == &InputSection<ELFT>::Discarded)
    return nullptr;

  if (!Target)
    fatal("unsupported relocation reference");
  return Target;
}

template <class ELFT>
InputSectionBase<ELFT> *
elf::ObjectFile<ELFT>::createInputSection(const Elf_Shdr &Sec) {
  StringRef Name = check(this->ELFObj.getSectionName(&Sec));

  // .note.GNU-stack is a marker section to control the presence of
  // PT_GNU_STACK segment in outputs. Since the presence of the segment
  // is controlled only by the command line option (-z execstack) in LLD,
  // .note.GNU-stack is ignored.
  if (Name == ".note.GNU-stack")
    return &InputSection<ELFT>::Discarded;

  if (Name == ".note.GNU-split-stack")
    error("objects using splitstacks are not supported");

  // A MIPS object file has a special section that contains register
  // usage info, which needs to be handled by the linker specially.
  if (Config->EMachine == EM_MIPS && Name == ".reginfo") {
    MipsReginfo = new (Alloc) MipsReginfoInputSection<ELFT>(this, &Sec);
    return MipsReginfo;
  }

  // We dont need special handling of .eh_frame sections if relocatable
  // output was choosen. Proccess them as usual input sections.
  if (!Config->Relocatable && Name == ".eh_frame")
    return new (EHAlloc.Allocate()) EHInputSection<ELFT>(this, &Sec);
  if (shouldMerge<ELFT>(Sec))
    return new (MAlloc.Allocate()) MergeInputSection<ELFT>(this, &Sec);
  return new (Alloc) InputSection<ELFT>(this, &Sec);
}

template <class ELFT> void elf::ObjectFile<ELFT>::initializeSymbols() {
  this->initStringTable();
  Elf_Sym_Range Syms = this->getElfSymbols(false);
  uint32_t NumSymbols = std::distance(Syms.begin(), Syms.end());
  SymbolBodies.reserve(NumSymbols);
  for (const Elf_Sym &Sym : Syms)
    SymbolBodies.push_back(createSymbolBody(&Sym));
}

template <class ELFT>
InputSectionBase<ELFT> *
elf::ObjectFile<ELFT>::getSection(const Elf_Sym &Sym) const {
  uint32_t Index = this->getSectionIndex(Sym);
  if (Index == 0)
    return nullptr;
  if (Index >= Sections.size() || !Sections[Index])
    fatal("invalid section index");
  InputSectionBase<ELFT> *S = Sections[Index];
  if (S == &InputSectionBase<ELFT>::Discarded)
    return S;
  return S->Repl;
}

template <class ELFT>
SymbolBody *elf::ObjectFile<ELFT>::createSymbolBody(const Elf_Sym *Sym) {
  uint32_t NameOffset = Sym->st_name;
  unsigned char Binding = Sym->getBinding();
  InputSectionBase<ELFT> *Sec = getSection(*Sym);
  if (Binding == STB_LOCAL) {
    if (Sym->st_shndx == SHN_UNDEF)
      return new (Alloc) UndefinedElf<ELFT>(NameOffset, *Sym);
    return new (Alloc) DefinedRegular<ELFT>(NameOffset, *Sym, Sec);
  }

  StringRef Name = check(Sym->getName(this->StringTable));

  switch (Sym->st_shndx) {
  case SHN_UNDEF:
    return new (Alloc) UndefinedElf<ELFT>(Name, *Sym);
  case SHN_COMMON:
    return new (Alloc) DefinedCommon(Name, Sym->st_size, Sym->st_value, Binding,
                                     Sym->st_other, Sym->getType());
  }

  switch (Binding) {
  default:
    fatal("unexpected binding");
  case STB_GLOBAL:
  case STB_WEAK:
  case STB_GNU_UNIQUE:
    if (Sec == &InputSection<ELFT>::Discarded)
      return new (Alloc) UndefinedElf<ELFT>(Name, *Sym);
    return new (Alloc) DefinedRegular<ELFT>(Name, *Sym, Sec);
  }
}

void ArchiveFile::parse() {
  File = check(Archive::create(MB), "failed to parse archive");

  // Allocate a buffer for Lazy objects.
  size_t NumSyms = File->getNumberOfSymbols();
  LazySymbols.reserve(NumSyms);

  // Read the symbol table to construct Lazy objects.
  for (const Archive::Symbol &Sym : File->symbols())
    LazySymbols.emplace_back(this, Sym);
}

// Returns a buffer pointing to a member file containing a given symbol.
MemoryBufferRef ArchiveFile::getMember(const Archive::Symbol *Sym) {
  Archive::Child C =
      check(Sym->getMember(),
            "could not get the member for symbol " + Sym->getName());

  if (!Seen.insert(C.getChildOffset()).second)
    return MemoryBufferRef();

  return check(C.getMemoryBufferRef(),
               "could not get the buffer for the member defining symbol " +
                   Sym->getName());
}

template <class ELFT>
SharedFile<ELFT>::SharedFile(MemoryBufferRef M)
    : ELFFileBase<ELFT>(Base::SharedKind, M), AsNeeded(Config->AsNeeded) {}

template <class ELFT>
const typename ELFT::Shdr *
SharedFile<ELFT>::getSection(const Elf_Sym &Sym) const {
  uint32_t Index = this->getSectionIndex(Sym);
  if (Index == 0)
    return nullptr;
  return check(this->ELFObj.getSection(Index));
}

// Partially parse the shared object file so that we can call
// getSoName on this object.
template <class ELFT> void SharedFile<ELFT>::parseSoName() {
  typedef typename ELFT::Dyn Elf_Dyn;
  typedef typename ELFT::uint uintX_t;
  const Elf_Shdr *DynamicSec = nullptr;

  const ELFFile<ELFT> Obj = this->ELFObj;
  for (const Elf_Shdr &Sec : Obj.sections()) {
    switch (Sec.sh_type) {
    default:
      continue;
    case SHT_DYNSYM:
      this->Symtab = &Sec;
      break;
    case SHT_DYNAMIC:
      DynamicSec = &Sec;
      break;
    case SHT_SYMTAB_SHNDX:
      this->SymtabSHNDX = check(Obj.getSHNDXTable(Sec));
      break;
    }
  }

  this->initStringTable();
  SoName = this->getName();

  if (!DynamicSec)
    return;
  auto *Begin =
      reinterpret_cast<const Elf_Dyn *>(Obj.base() + DynamicSec->sh_offset);
  const Elf_Dyn *End = Begin + DynamicSec->sh_size / sizeof(Elf_Dyn);

  for (const Elf_Dyn &Dyn : make_range(Begin, End)) {
    if (Dyn.d_tag == DT_SONAME) {
      uintX_t Val = Dyn.getVal();
      if (Val >= this->StringTable.size())
        fatal("invalid DT_SONAME entry");
      SoName = StringRef(this->StringTable.data() + Val);
      return;
    }
  }
}

// Fully parse the shared object file. This must be called after parseSoName().
template <class ELFT> void SharedFile<ELFT>::parseRest() {
  Elf_Sym_Range Syms = this->getElfSymbols(true);
  uint32_t NumSymbols = std::distance(Syms.begin(), Syms.end());
  SymbolBodies.reserve(NumSymbols);
  for (const Elf_Sym &Sym : Syms) {
    // FIXME: We should probably just err if we get a local symbol in here.
    if (Sym.getBinding() == STB_LOCAL)
      continue;
    StringRef Name = check(Sym.getName(this->StringTable));
    if (Sym.isUndefined())
      Undefs.push_back(Name);
    else
      SymbolBodies.emplace_back(this, Name, Sym);
  }
}

BitcodeFile::BitcodeFile(MemoryBufferRef M) : InputFile(BitcodeKind, M) {}

bool BitcodeFile::classof(const InputFile *F) {
  return F->kind() == BitcodeKind;
}

static uint8_t getGvVisibility(const GlobalValue *GV) {
  switch (GV->getVisibility()) {
  case GlobalValue::DefaultVisibility:
    return STV_DEFAULT;
  case GlobalValue::HiddenVisibility:
    return STV_HIDDEN;
  case GlobalValue::ProtectedVisibility:
    return STV_PROTECTED;
  }
  llvm_unreachable("unknown visibility");
}

SymbolBody *
BitcodeFile::createSymbolBody(const DenseSet<const Comdat *> &KeptComdats,
                              const IRObjectFile &Obj,
                              const BasicSymbolRef &Sym) {
  const GlobalValue *GV = Obj.getSymbolGV(Sym.getRawDataRefImpl());
  assert(GV);
  if (const Comdat *C = GV->getComdat())
    if (!KeptComdats.count(C))
      return nullptr;

  uint8_t Visibility = getGvVisibility(GV);

  SmallString<64> Name;
  raw_svector_ostream OS(Name);
  Sym.printName(OS);
  StringRef NameRef = Saver.save(StringRef(Name));

  const Module &M = Obj.getModule();
  SymbolBody *Body;
  uint32_t Flags = Sym.getFlags();
  bool IsWeak = Flags & BasicSymbolRef::SF_Weak;
  if (Flags & BasicSymbolRef::SF_Undefined) {
    Body = new (Alloc) Undefined(NameRef, IsWeak, Visibility, false);
  } else if (Flags & BasicSymbolRef::SF_Common) {
    const DataLayout &DL = M.getDataLayout();
    uint64_t Size = DL.getTypeAllocSize(GV->getValueType());
    Body = new (Alloc)
        DefinedCommon(NameRef, Size, GV->getAlignment(),
                      IsWeak ? STB_WEAK : STB_GLOBAL, Visibility, /*Type*/ 0);
  } else {
    Body = new (Alloc) DefinedBitcode(NameRef, IsWeak, Visibility);
  }
  if (GV->isThreadLocal())
    Body->Type = STT_TLS;
  return Body;
}

bool BitcodeFile::shouldSkip(const BasicSymbolRef &Sym) {
  uint32_t Flags = Sym.getFlags();
  if (!(Flags & BasicSymbolRef::SF_Global))
    return true;
  if (Flags & BasicSymbolRef::SF_FormatSpecific)
    return true;
  return false;
}

void BitcodeFile::parse(DenseSet<StringRef> &ComdatGroups) {
  LLVMContext Context;
  std::unique_ptr<IRObjectFile> Obj = check(IRObjectFile::create(MB, Context));
  const Module &M = Obj->getModule();

  DenseSet<const Comdat *> KeptComdats;
  for (const auto &P : M.getComdatSymbolTable()) {
    StringRef N = Saver.save(P.first());
    if (ComdatGroups.insert(N).second)
      KeptComdats.insert(&P.second);
  }

  for (const BasicSymbolRef &Sym : Obj->symbols())
    if (!shouldSkip(Sym))
      SymbolBodies.push_back(createSymbolBody(KeptComdats, *Obj, Sym));
}

template <typename T>
static std::unique_ptr<InputFile> createELFFileAux(MemoryBufferRef MB) {
  std::unique_ptr<T> Ret = llvm::make_unique<T>(MB);

  if (!Config->FirstElf)
    Config->FirstElf = Ret.get();

  if (Config->EKind == ELFNoneKind) {
    Config->EKind = Ret->getELFKind();
    Config->EMachine = Ret->getEMachine();
  }

  return std::move(Ret);
}

template <template <class> class T>
static std::unique_ptr<InputFile> createELFFile(MemoryBufferRef MB) {
  std::pair<unsigned char, unsigned char> Type = getElfArchType(MB.getBuffer());
  if (Type.second != ELF::ELFDATA2LSB && Type.second != ELF::ELFDATA2MSB)
    fatal("invalid data encoding: " + MB.getBufferIdentifier());

  if (Type.first == ELF::ELFCLASS32) {
    if (Type.second == ELF::ELFDATA2LSB)
      return createELFFileAux<T<ELF32LE>>(MB);
    return createELFFileAux<T<ELF32BE>>(MB);
  }
  if (Type.first == ELF::ELFCLASS64) {
    if (Type.second == ELF::ELFDATA2LSB)
      return createELFFileAux<T<ELF64LE>>(MB);
    return createELFFileAux<T<ELF64BE>>(MB);
  }
  fatal("invalid file class: " + MB.getBufferIdentifier());
}

std::unique_ptr<InputFile> elf::createObjectFile(MemoryBufferRef MB,
                                                 StringRef ArchiveName) {
  using namespace sys::fs;
  std::unique_ptr<InputFile> F;
  if (identify_magic(MB.getBuffer()) == file_magic::bitcode)
    F.reset(new BitcodeFile(MB));
  else
    F = createELFFile<ObjectFile>(MB);
  F->ArchiveName = ArchiveName;
  return F;
}

std::unique_ptr<InputFile> elf::createSharedFile(MemoryBufferRef MB) {
  return createELFFile<SharedFile>(MB);
}

template class elf::ELFFileBase<ELF32LE>;
template class elf::ELFFileBase<ELF32BE>;
template class elf::ELFFileBase<ELF64LE>;
template class elf::ELFFileBase<ELF64BE>;

template class elf::ObjectFile<ELF32LE>;
template class elf::ObjectFile<ELF32BE>;
template class elf::ObjectFile<ELF64LE>;
template class elf::ObjectFile<ELF64BE>;

template class elf::SharedFile<ELF32LE>;
template class elf::SharedFile<ELF32BE>;
template class elf::SharedFile<ELF64LE>;
template class elf::SharedFile<ELF64BE>;
