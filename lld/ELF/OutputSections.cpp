//===- OutputSections.cpp -------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "OutputSections.h"
#include "Config.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "SymbolTable.h"

#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

using namespace lld;
using namespace lld::elf2;

template <bool Is64Bits>
OutputSectionBase<Is64Bits>::OutputSectionBase(StringRef Name, uint32_t sh_type,
                                               uintX_t sh_flags)
    : Name(Name) {
  memset(&Header, 0, sizeof(HeaderT));
  Header.sh_type = sh_type;
  Header.sh_flags = sh_flags;
}

template <class ELFT> void GotSection<ELFT>::addEntry(SymbolBody *Sym) {
  Sym->setGotIndex(Entries.size());
  Entries.push_back(Sym);
}

template <class ELFT>
typename GotSection<ELFT>::uintX_t
GotSection<ELFT>::getEntryAddr(const SymbolBody &B) const {
  return this->getVA() + B.getGotIndex() * this->getAddrSize();
}

template <class ELFT> void PltSection<ELFT>::writeTo(uint8_t *Buf) {
  uintptr_t Start = reinterpret_cast<uintptr_t>(Buf);
  ArrayRef<uint8_t> Jmp = {0xff, 0x25}; // jmpq *val(%rip)
  for (const SymbolBody *E : Entries) {
    uintptr_t InstPos = reinterpret_cast<uintptr_t>(Buf);

    memcpy(Buf, Jmp.data(), Jmp.size());
    Buf += Jmp.size();

    uintptr_t OffsetInPLT = (InstPos + 6) - Start;
    uintptr_t Delta = GotSec.getEntryAddr(*E) - (this->getVA() + OffsetInPLT);
    assert(isInt<32>(Delta));
    support::endian::write32le(Buf, Delta);
    Buf += 4;

    *Buf = 0x90; // nop
    ++Buf;
    *Buf = 0x90; // nop
    ++Buf;
  }
}

template <class ELFT> void PltSection<ELFT>::addEntry(SymbolBody *Sym) {
  Sym->setPltIndex(Entries.size());
  Entries.push_back(Sym);
}

template <class ELFT>
typename PltSection<ELFT>::uintX_t
PltSection<ELFT>::getEntryAddr(const SymbolBody &B) const {
  return this->getVA() + B.getPltIndex() * EntrySize;
}

bool lld::elf2::relocNeedsPLT(uint32_t Type) {
  switch (Type) {
  default:
    return false;
  case R_X86_64_PLT32:
    return true;
  }
}

bool lld::elf2::relocNeedsGOT(uint32_t Type) {
  if (relocNeedsPLT(Type))
    return true;
  switch (Type) {
  default:
    return false;
  case R_X86_64_GOTPCREL:
    return true;
  }
}

template <class ELFT> void RelocationSection<ELFT>::writeTo(uint8_t *Buf) {
  auto *P = reinterpret_cast<Elf_Rela *>(Buf);
  bool IsMips64EL = Relocs[0].C.getFile()->getObj()->isMips64EL();
  for (const DynamicReloc<ELFT> &Rel : Relocs) {
    const InputSection<ELFT> &C = Rel.C;
    const Elf_Rel &RI = Rel.RI;
    OutputSection<ELFT> *Out = C.getOutputSection();
    uint32_t SymIndex = RI.getSymbol(IsMips64EL);
    const SymbolBody *Body = C.getFile()->getSymbolBody(SymIndex);
    uint32_t Type = RI.getType(IsMips64EL);
    if (relocNeedsGOT(Type)) {
      P->r_offset = GotSec.getEntryAddr(*Body);
      P->setSymbolAndType(Body->getDynamicSymbolTableIndex(), R_X86_64_GLOB_DAT,
                          IsMips64EL);
    } else {
      P->r_offset = RI.r_offset + C.getOutputSectionOff() + Out->getVA();
      P->setSymbolAndType(Body->getDynamicSymbolTableIndex(), Type, IsMips64EL);
      if (IsRela)
        P->r_addend = static_cast<const Elf_Rela &>(RI).r_addend;
    }

    ++P;
  }
}

template <class ELFT> void RelocationSection<ELFT>::finalize() {
  this->Header.sh_link = DynSymSec.getSectionIndex();
  this->Header.sh_size = Relocs.size() * this->Header.sh_entsize;
}

template <bool Is64Bits>
InterpSection<Is64Bits>::InterpSection()
    : OutputSectionBase<Is64Bits>(".interp", llvm::ELF::SHT_PROGBITS,
                                  llvm::ELF::SHF_ALLOC) {
  this->Header.sh_size = Config->DynamicLinker.size() + 1;
  this->Header.sh_addralign = 1;
}

template <bool Is64Bits>
template <endianness E>
void OutputSectionBase<Is64Bits>::writeHeaderTo(
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

template <bool Is64Bits> void InterpSection<Is64Bits>::writeTo(uint8_t *Buf) {
  memcpy(Buf, Config->DynamicLinker.data(), Config->DynamicLinker.size());
}

template <class ELFT> void HashTableSection<ELFT>::addSymbol(SymbolBody *S) {
  StringRef Name = S->getName();
  DynSymSec.addSymbol(Name);
  Hashes.push_back(hash(Name));
  S->setDynamicSymbolTableIndex(Hashes.size());
}

template <class ELFT> void DynamicSection<ELFT>::finalize() {
  typename Base::HeaderT &Header = this->Header;
  Header.sh_link = DynStrSec.getSectionIndex();

  unsigned NumEntries = 0;
  if (RelaDynSec.hasRelocs()) {
    ++NumEntries; // DT_RELA / DT_REL
    ++NumEntries; // DT_RELASZ / DTRELSZ
  }
  ++NumEntries; // DT_SYMTAB
  ++NumEntries; // DT_STRTAB
  ++NumEntries; // DT_STRSZ
  ++NumEntries; // DT_HASH

  StringRef RPath = Config->RPath;
  if (!RPath.empty()) {
    ++NumEntries; // DT_RUNPATH
    DynStrSec.add(RPath);
  }

  const std::vector<std::unique_ptr<SharedFileBase>> &SharedFiles =
      SymTab.getSharedFiles();
  for (const std::unique_ptr<SharedFileBase> &File : SharedFiles)
    DynStrSec.add(File->getName());
  NumEntries += SharedFiles.size();

  ++NumEntries; // DT_NULL

  Header.sh_size = NumEntries * Header.sh_entsize;
}

template <class ELFT> void DynamicSection<ELFT>::writeTo(uint8_t *Buf) {
  typedef typename std::conditional<ELFT::Is64Bits, Elf64_Dyn, Elf32_Dyn>::type
      Elf_Dyn;
  auto *P = reinterpret_cast<Elf_Dyn *>(Buf);

  if (RelaDynSec.hasRelocs()) {
    bool IsRela = RelaDynSec.isRela();
    P->d_tag = IsRela ? DT_RELA : DT_REL;
    P->d_un.d_ptr = RelaDynSec.getVA();
    ++P;

    P->d_tag = IsRela ? DT_RELASZ : DT_RELSZ;
    P->d_un.d_val = RelaDynSec.getSize();
    ++P;
  }

  P->d_tag = DT_SYMTAB;
  P->d_un.d_ptr = DynSymSec.getVA();
  ++P;

  P->d_tag = DT_STRTAB;
  P->d_un.d_ptr = DynStrSec.getVA();
  ++P;

  P->d_tag = DT_STRSZ;
  P->d_un.d_val = DynStrSec.data().size();
  ++P;

  P->d_tag = DT_HASH;
  P->d_un.d_ptr = HashSec.getVA();
  ++P;

  StringRef RPath = Config->RPath;
  if (!RPath.empty()) {
    P->d_tag = DT_RUNPATH;
    P->d_un.d_val = DynStrSec.getFileOff(RPath);
    ++P;
  }

  const std::vector<std::unique_ptr<SharedFileBase>> &SharedFiles =
      SymTab.getSharedFiles();
  for (const std::unique_ptr<SharedFileBase> &File : SharedFiles) {
    P->d_tag = DT_NEEDED;
    P->d_un.d_val = DynStrSec.getFileOff(File->getName());
    ++P;
  }

  P->d_tag = DT_NULL;
  P->d_un.d_val = 0;
  ++P;
}

template <class ELFT>
void OutputSection<ELFT>::addChunk(InputSection<ELFT> *C) {
  Chunks.push_back(C);
  C->setOutputSection(this);
  uint32_t Align = C->getAlign();
  if (Align > this->Header.sh_addralign)
    this->Header.sh_addralign = Align;

  uintX_t Off = this->Header.sh_size;
  Off = RoundUpToAlignment(Off, Align);
  C->setOutputSectionOff(Off);
  Off += C->getSize();
  this->Header.sh_size = Off;
}

template <class ELFT>
void OutputSection<ELFT>::relocateOne(uint8_t *Buf, const Elf_Rel &Rel,
                                      uint32_t Type, uintX_t BaseAddr,
                                      uintX_t SymVA) {
  uintX_t Offset = Rel.r_offset;
  uint8_t *Location = Buf + Offset;
  switch (Type) {
  case R_386_32:
    support::endian::write32le(Location, SymVA);
    break;
  default:
    llvm::errs() << Twine("unrecognized reloc ") + Twine(Type) << '\n';
    break;
  }
}

template <class ELFT>
void OutputSection<ELFT>::relocateOne(uint8_t *Buf, const Elf_Rela &Rel,
                                      uint32_t Type, uintX_t BaseAddr,
                                      uintX_t SymVA) {
  uintX_t Offset = Rel.r_offset;
  uint8_t *Location = Buf + Offset;
  switch (Type) {
  case R_X86_64_PC32:
    support::endian::write32le(Location,
                               SymVA + (Rel.r_addend - (BaseAddr + Offset)));
    break;
  case R_X86_64_64:
    support::endian::write64le(Location, SymVA + Rel.r_addend);
    break;
  case R_X86_64_32: {
  case R_X86_64_32S:
    uint64_t VA = SymVA + Rel.r_addend;
    if (Type == R_X86_64_32 && !isUInt<32>(VA))
      error("R_X86_64_32 out of range");
    else if (!isInt<32>(VA))
      error("R_X86_64_32S out of range");

    support::endian::write32le(Location, VA);
    break;
  }
  default:
    llvm::errs() << Twine("unrecognized reloc ") + Twine(Type) << '\n';
    break;
  }
}

template <class ELFT>
typename ELFFile<ELFT>::uintX_t
lld::elf2::getSymVA(const DefinedRegular<ELFT> *DR) {
  const InputSection<ELFT> *SC = &DR->Section;
  OutputSection<ELFT> *OS = SC->getOutputSection();
  return OS->getVA() + SC->getOutputSectionOff() + DR->Sym.st_value;
}

template <class ELFT>
typename ELFFile<ELFT>::uintX_t
lld::elf2::getLocalSymVA(const typename ELFFile<ELFT>::Elf_Sym *Sym,
                         const ObjectFile<ELFT> &File) {
  uint32_t SecIndex = Sym->st_shndx;

  if (SecIndex == SHN_XINDEX)
    SecIndex = File.getObj()->getExtendedSymbolTableIndex(
        Sym, File.getSymbolTable(), File.getSymbolTableShndx());
  ArrayRef<InputSection<ELFT> *> Chunks = File.getChunks();
  InputSection<ELFT> *Section = Chunks[SecIndex];
  OutputSection<ELFT> *Out = Section->getOutputSection();
  return Out->getVA() + Section->getOutputSectionOff() + Sym->st_value;
}

template <class ELFT>
template <bool isRela>
void OutputSection<ELFT>::relocate(
    uint8_t *Buf, iterator_range<const Elf_Rel_Impl<ELFT, isRela> *> Rels,
    const ObjectFile<ELFT> &File, uintX_t BaseAddr) {
  typedef Elf_Rel_Impl<ELFT, isRela> RelType;
  bool IsMips64EL = File.getObj()->isMips64EL();
  for (const RelType &RI : Rels) {
    uint32_t SymIndex = RI.getSymbol(IsMips64EL);
    uint32_t Type = RI.getType(IsMips64EL);
    uintX_t SymVA;

    // Handle relocations for local symbols -- they never get
    // resolved so we don't allocate a SymbolBody.
    const Elf_Shdr *SymTab = File.getSymbolTable();
    if (SymIndex < SymTab->sh_info) {
      const Elf_Sym *Sym = File.getObj()->getRelocationSymbol(&RI, SymTab);
      if (!Sym)
        continue;
      SymVA = getLocalSymVA(Sym, File);
    } else {
      const SymbolBody *Body = File.getSymbolBody(SymIndex);
      if (!Body)
        continue;
      switch (Body->kind()) {
      case SymbolBody::DefinedRegularKind:
        SymVA = getSymVA<ELFT>(cast<DefinedRegular<ELFT>>(Body));
        break;
      case SymbolBody::DefinedAbsoluteKind:
        SymVA = cast<DefinedAbsolute<ELFT>>(Body)->Sym.st_value;
        break;
      case SymbolBody::DefinedCommonKind: {
        auto *DC = cast<DefinedCommon<ELFT>>(Body);
        SymVA = DC->OutputSec->getVA() + DC->OffsetInBSS;
        break;
      }
      case SymbolBody::SharedKind:
        if (relocNeedsPLT(Type)) {
          SymVA = PltSec.getEntryAddr(*Body);
          Type = R_X86_64_PC32;
        } else if (relocNeedsGOT(Type)) {
          SymVA = GotSec.getEntryAddr(*Body);
          Type = R_X86_64_PC32;
        } else {
          continue;
        }
        break;
      case SymbolBody::UndefinedKind:
        assert(Body->isWeak() && "Undefined symbol reached writer");
        SymVA = 0;
        break;
      case SymbolBody::LazyKind:
        llvm_unreachable("Lazy symbol reached writer");
      }
    }

    relocateOne(Buf, RI, Type, BaseAddr, SymVA);
  }
}

template <class ELFT> void OutputSection<ELFT>::writeTo(uint8_t *Buf) {
  for (InputSection<ELFT> *C : Chunks) {
    C->writeTo(Buf);
    const ObjectFile<ELFT> *File = C->getFile();
    ELFFile<ELFT> *EObj = File->getObj();
    uint8_t *Base = Buf + C->getOutputSectionOff();
    uintX_t BaseAddr = this->getVA() + C->getOutputSectionOff();
    // Iterate over all relocation sections that apply to this section.
    for (const Elf_Shdr *RelSec : C->RelocSections) {
      if (RelSec->sh_type == SHT_RELA)
        relocate(Base, EObj->relas(RelSec), *File, BaseAddr);
      else
        relocate(Base, EObj->rels(RelSec), *File, BaseAddr);
    }
  }
}

template <bool Is64Bits>
void StringTableSection<Is64Bits>::writeTo(uint8_t *Buf) {
  StringRef Data = StrTabBuilder.data();
  memcpy(Buf, Data.data(), Data.size());
}

bool lld::elf2::includeInSymtab(const SymbolBody &B) {
  if (B.isLazy())
    return false;
  if (!B.isUsedInRegularObj())
    return false;
  uint8_t V = B.getMostConstrainingVisibility();
  if (V != STV_DEFAULT && V != STV_PROTECTED)
    return false;
  return true;
}

template <class ELFT> void SymbolTableSection<ELFT>::writeTo(uint8_t *Buf) {
  const OutputSection<ELFT> *Out = nullptr;
  const InputSection<ELFT> *Section = nullptr;
  Buf += sizeof(Elf_Sym);

  // All symbols with STB_LOCAL binding precede the weak and global symbols.
  // .dynsym only contains global symbols.
  if (!Config->DiscardAll && !StrTabSec.isDynamic()) {
    for (const std::unique_ptr<ObjectFileBase> &FileB :
         Table.getObjectFiles()) {
      auto &File = cast<ObjectFile<ELFT>>(*FileB);
      Elf_Sym_Range Syms = File.getLocalSymbols();
      for (const Elf_Sym &Sym : Syms) {
        auto *ESym = reinterpret_cast<Elf_Sym *>(Buf);
        uint32_t SecIndex = Sym.st_shndx;
        ErrorOr<StringRef> SymName = Sym.getName(File.getStringTable());
        if (Config->DiscardLocals && SymName->startswith(".L"))
          continue;
        ESym->st_name = (SymName) ? StrTabSec.getFileOff(*SymName) : 0;
        ESym->st_size = Sym.st_size;
        ESym->setBindingAndType(Sym.getBinding(), Sym.getType());
        if (SecIndex == SHN_XINDEX)
          SecIndex = File.getObj()->getExtendedSymbolTableIndex(
              &Sym, File.getSymbolTable(), File.getSymbolTableShndx());
        ArrayRef<InputSection<ELFT> *> Chunks = File.getChunks();
        Section = Chunks[SecIndex];
        assert(Section != nullptr);
        Out = Section->getOutputSection();
        assert(Out != nullptr);
        ESym->st_shndx = Out->getSectionIndex();
        ESym->st_value =
            Out->getVA() + Section->getOutputSectionOff() + Sym.st_value;
        Buf += sizeof(Elf_Sym);
      }
    }
  }

  for (auto &P : Table.getSymbols()) {
    StringRef Name = P.first;
    Symbol *Sym = P.second;
    SymbolBody *Body = Sym->Body;
    if (!includeInSymtab(*Body))
      continue;
    const Elf_Sym &InputSym = cast<ELFSymbolBody<ELFT>>(Body)->Sym;

    auto *ESym = reinterpret_cast<Elf_Sym *>(Buf);
    ESym->st_name = StrTabSec.getFileOff(Name);

    Out = nullptr;
    Section = nullptr;

    switch (Body->kind()) {
    case SymbolBody::DefinedRegularKind:
      Section = &cast<DefinedRegular<ELFT>>(Body)->Section;
      break;
    case SymbolBody::DefinedCommonKind:
      Out = BssSec;
      break;
    case SymbolBody::UndefinedKind:
    case SymbolBody::DefinedAbsoluteKind:
    case SymbolBody::SharedKind:
      break;
    case SymbolBody::LazyKind:
      llvm_unreachable("Lazy symbol got to output symbol table!");
    }

    ESym->setBindingAndType(InputSym.getBinding(), InputSym.getType());
    ESym->st_size = InputSym.st_size;
    ESym->setVisibility(Body->getMostConstrainingVisibility());
    if (InputSym.isAbsolute()) {
      ESym->st_shndx = SHN_ABS;
      ESym->st_value = InputSym.st_value;
    }

    if (Section)
      Out = Section->getOutputSection();

    if (Out) {
      ESym->st_shndx = Out->getSectionIndex();
      uintX_t VA = Out->getVA();
      if (Section)
        VA += Section->getOutputSectionOff();
      if (auto *C = dyn_cast<DefinedCommon<ELFT>>(Body))
        VA += C->OffsetInBSS;
      else
        VA += InputSym.st_value;
      ESym->st_value = VA;
    }

    Buf += sizeof(Elf_Sym);
  }
}

namespace lld {
namespace elf2 {
template class OutputSectionBase<false>;
template class OutputSectionBase<true>;

template void OutputSectionBase<false>::writeHeaderTo<support::little>(
    typename ELFFile<ELFType<support::little, false>>::Elf_Shdr *SHdr);
template void OutputSectionBase<true>::writeHeaderTo<support::little>(
    typename ELFFile<ELFType<support::little, true>>::Elf_Shdr *SHdr);
template void OutputSectionBase<false>::writeHeaderTo<support::big>(
    typename ELFFile<ELFType<support::big, false>>::Elf_Shdr *SHdr);
template void OutputSectionBase<true>::writeHeaderTo<support::big>(
    typename ELFFile<ELFType<support::big, true>>::Elf_Shdr *SHdr);

template class GotSection<ELF32LE>;
template class GotSection<ELF32BE>;
template class GotSection<ELF64LE>;
template class GotSection<ELF64BE>;

template class PltSection<ELF32LE>;
template class PltSection<ELF32BE>;
template class PltSection<ELF64LE>;
template class PltSection<ELF64BE>;

template class RelocationSection<ELF32LE>;
template class RelocationSection<ELF32BE>;
template class RelocationSection<ELF64LE>;
template class RelocationSection<ELF64BE>;

template class InterpSection<false>;
template class InterpSection<true>;

template class HashTableSection<ELF32LE>;
template class HashTableSection<ELF32BE>;
template class HashTableSection<ELF64LE>;
template class HashTableSection<ELF64BE>;

template class DynamicSection<ELF32LE>;
template class DynamicSection<ELF32BE>;
template class DynamicSection<ELF64LE>;
template class DynamicSection<ELF64BE>;

template class OutputSection<ELF32LE>;
template class OutputSection<ELF32BE>;
template class OutputSection<ELF64LE>;
template class OutputSection<ELF64BE>;

template class StringTableSection<false>;
template class StringTableSection<true>;

template class SymbolTableSection<ELF32LE>;
template class SymbolTableSection<ELF32BE>;
template class SymbolTableSection<ELF64LE>;
template class SymbolTableSection<ELF64BE>;

template typename ELFFile<ELF32LE>::uintX_t
getSymVA(const DefinedRegular<ELF32LE> *DR);

template typename ELFFile<ELF32BE>::uintX_t
getSymVA(const DefinedRegular<ELF32BE> *DR);

template typename ELFFile<ELF64LE>::uintX_t
getSymVA(const DefinedRegular<ELF64LE> *DR);

template typename ELFFile<ELF64BE>::uintX_t
getSymVA(const DefinedRegular<ELF64BE> *DR);
}
}
