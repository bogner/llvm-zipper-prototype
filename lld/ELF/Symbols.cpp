//===- Symbols.cpp --------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"
#include "Error.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "OutputSections.h"
#include "Target.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Config/config.h"

#ifdef HAVE_CXXABI_H
#include <cxxabi.h>
#endif

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

using namespace lld;
using namespace lld::elf;

template <class ELFT>
typename ELFFile<ELFT>::uintX_t SymbolBody::getVA() const {
  switch (kind()) {
  case DefinedSyntheticKind: {
    auto *D = cast<DefinedSynthetic<ELFT>>(this);
    return D->Section.getVA() + D->Value;
  }
  case DefinedRegularKind: {
    auto *D = cast<DefinedRegular<ELFT>>(this);
    InputSectionBase<ELFT> *SC = D->Section;

    // This is an absolute symbol.
    if (!SC)
      return D->Sym.st_value;
    assert(SC->Live);

    // Symbol offsets for AMDGPU are the offsets in bytes of the symbols
    // from the beginning of the section. Note that this part of AMDGPU's
    // ELF spec is odd and not in line with the standard ELF.
    if (Config->EMachine == EM_AMDGPU)
      return SC->getOffset(D->Sym);

    if (D->Sym.getType() == STT_TLS)
      return SC->OutSec->getVA() + SC->getOffset(D->Sym) -
             Out<ELFT>::TlsPhdr->p_vaddr;
    return SC->OutSec->getVA() + SC->getOffset(D->Sym);
  }
  case DefinedCommonKind:
    return Out<ELFT>::Bss->getVA() + cast<DefinedCommon>(this)->OffsetInBss;
  case SharedKind: {
    auto *SS = cast<SharedSymbol<ELFT>>(this);
    if (!SS->NeedsCopyOrPltAddr)
      return 0;
    if (SS->IsFunc)
      return getPltVA<ELFT>();
    else
      return Out<ELFT>::Bss->getVA() + SS->OffsetInBss;
  }
  case UndefinedElfKind:
  case UndefinedKind:
    return 0;
  case LazyKind:
    assert(isUsedInRegularObj() && "Lazy symbol reached writer");
    return 0;
  case DefinedBitcodeKind:
    llvm_unreachable("Should have been replaced");
  }
  llvm_unreachable("Invalid symbol kind");
}

template <class ELFT>
typename ELFFile<ELFT>::uintX_t SymbolBody::getGotVA() const {
  return Out<ELFT>::Got->getVA() +
         (Out<ELFT>::Got->getMipsLocalEntriesNum() + GotIndex) *
             sizeof(typename ELFFile<ELFT>::uintX_t);
}

template <class ELFT>
typename ELFFile<ELFT>::uintX_t SymbolBody::getGotPltVA() const {
  return Out<ELFT>::GotPlt->getVA() +
         GotPltIndex * sizeof(typename ELFFile<ELFT>::uintX_t);
}

template <class ELFT>
typename ELFFile<ELFT>::uintX_t SymbolBody::getPltVA() const {
  return Out<ELFT>::Plt->getVA() + Target->PltZeroSize +
         PltIndex * Target->PltEntrySize;
}

template <class ELFT>
typename ELFFile<ELFT>::uintX_t SymbolBody::getSize() const {
  if (auto *B = dyn_cast<DefinedElf<ELFT>>(this))
    return B->Sym.st_size;
  return 0;
}

static uint8_t getMinVisibility(uint8_t VA, uint8_t VB) {
  if (VA == STV_DEFAULT)
    return VB;
  if (VB == STV_DEFAULT)
    return VA;
  return std::min(VA, VB);
}

// Returns 1, 0 or -1 if this symbol should take precedence
// over the Other, tie or lose, respectively.
template <class ELFT> int SymbolBody::compare(SymbolBody *Other) {
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;
  assert(!isLazy() && !Other->isLazy());
  std::tuple<bool, bool, bool> L(isDefined(), !isShared(), !isWeak());
  std::tuple<bool, bool, bool> R(Other->isDefined(), !Other->isShared(),
                                 !Other->isWeak());

  // Normalize
  if (L > R)
    return -Other->compare<ELFT>(this);

  Visibility = Other->Visibility =
      getMinVisibility(Visibility, Other->Visibility);

  if (IsUsedInRegularObj || Other->IsUsedInRegularObj)
    IsUsedInRegularObj = Other->IsUsedInRegularObj = true;

  // We want to export all symbols that exist both in the executable
  // and in DSOs, so that the symbols in the executable can interrupt
  // symbols in the DSO at runtime.
  if (isShared() != Other->isShared())
    if (isa<DefinedRegular<ELFT>>(isShared() ? Other : this))
      MustBeInDynSym = Other->MustBeInDynSym = true;

  if (L != R)
    return -1;
  if (!std::get<0>(L) || !std::get<1>(L) || !std::get<2>(L))
    return 1;
  if (isCommon()) {
    if (!Other->isCommon())
      return -1;
    auto *ThisC = cast<DefinedCommon>(this);
    auto *OtherC = cast<DefinedCommon>(Other);
    uintX_t Align = std::max(ThisC->MaxAlignment, OtherC->MaxAlignment);
    if (ThisC->Size >= OtherC->Size) {
      ThisC->MaxAlignment = Align;
      return 1;
    }
    OtherC->MaxAlignment = Align;
    return -1;
  }
  if (Other->isCommon())
    return 1;
  return 0;
}

Defined::Defined(Kind K, StringRef Name, bool IsWeak, uint8_t Visibility,
                 uint8_t Type)
    : SymbolBody(K, Name, IsWeak, Visibility, Type) {}

DefinedBitcode::DefinedBitcode(StringRef Name, bool IsWeak)
    : Defined(DefinedBitcodeKind, Name, IsWeak, STV_DEFAULT, 0 /* Type */) {}

bool DefinedBitcode::classof(const SymbolBody *S) {
  return S->kind() == DefinedBitcodeKind;
}

Undefined::Undefined(SymbolBody::Kind K, StringRef N, bool IsWeak,
                     uint8_t Visibility, uint8_t Type)
    : SymbolBody(K, N, IsWeak, Visibility, Type),
      CanKeepUndefined(false) {}

Undefined::Undefined(StringRef N, bool IsWeak, uint8_t Visibility,
                     bool CanKeepUndefined)
    : Undefined(SymbolBody::UndefinedKind, N, IsWeak, Visibility, 0 /* Type */) {
  this->CanKeepUndefined = CanKeepUndefined;
}

template <typename ELFT>
UndefinedElf<ELFT>::UndefinedElf(StringRef N, const Elf_Sym &Sym)
    : Undefined(SymbolBody::UndefinedElfKind, N,
                Sym.getBinding() == llvm::ELF::STB_WEAK, Sym.getVisibility(),
                Sym.getType()),
      Sym(Sym) {}

template <typename ELFT>
DefinedSynthetic<ELFT>::DefinedSynthetic(StringRef N, uintX_t Value,
                                         OutputSectionBase<ELFT> &Section,
                                         uint8_t Visibility)
    : Defined(SymbolBody::DefinedSyntheticKind, N, false, Visibility,
              0 /* Type */),
      Value(Value), Section(Section) {}

DefinedCommon::DefinedCommon(StringRef N, uint64_t Size, uint64_t Alignment,
                             bool IsWeak, uint8_t Visibility)
    : Defined(SymbolBody::DefinedCommonKind, N, IsWeak, Visibility,
              0 /* Type */) {
  MaxAlignment = Alignment;
  this->Size = Size;
}

std::unique_ptr<InputFile> Lazy::getMember() {
  MemoryBufferRef MBRef = File->getMember(&Sym);

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (MBRef.getBuffer().empty())
    return std::unique_ptr<InputFile>(nullptr);
  return createObjectFile(MBRef, File->getName());
}

template <class ELFT> static void doInitSymbols() {
  ElfSym<ELFT>::Etext.setBinding(STB_GLOBAL);
  ElfSym<ELFT>::Edata.setBinding(STB_GLOBAL);
  ElfSym<ELFT>::End.setBinding(STB_GLOBAL);
  ElfSym<ELFT>::Ignored.setBinding(STB_WEAK);
  ElfSym<ELFT>::Ignored.setVisibility(STV_HIDDEN);
}

void elf::initSymbols() {
  doInitSymbols<ELF32LE>();
  doInitSymbols<ELF32BE>();
  doInitSymbols<ELF64LE>();
  doInitSymbols<ELF64BE>();
}

// Returns the demangled C++ symbol name for Name.
std::string elf::demangle(StringRef Name) {
#if !defined(HAVE_CXXABI_H)
  return Name;
#else
  if (!Config->Demangle)
    return Name;

  // __cxa_demangle can be used to demangle strings other than symbol
  // names which do not necessarily start with "_Z". Name can be
  // either a C or C++ symbol. Don't call __cxa_demangle if the name
  // does not look like a C++ symbol name to avoid getting unexpected
  // result for a C symbol that happens to match a mangled type name.
  if (!Name.startswith("_Z"))
    return Name;

  char *Buf =
      abi::__cxa_demangle(Name.str().c_str(), nullptr, nullptr, nullptr);
  if (!Buf)
    return Name;
  std::string S(Buf);
  free(Buf);
  return S;
#endif
}

template uint32_t SymbolBody::template getVA<ELF32LE>() const;
template uint32_t SymbolBody::template getVA<ELF32BE>() const;
template uint64_t SymbolBody::template getVA<ELF64LE>() const;
template uint64_t SymbolBody::template getVA<ELF64BE>() const;

template uint32_t SymbolBody::template getGotVA<ELF32LE>() const;
template uint32_t SymbolBody::template getGotVA<ELF32BE>() const;
template uint64_t SymbolBody::template getGotVA<ELF64LE>() const;
template uint64_t SymbolBody::template getGotVA<ELF64BE>() const;

template uint32_t SymbolBody::template getGotPltVA<ELF32LE>() const;
template uint32_t SymbolBody::template getGotPltVA<ELF32BE>() const;
template uint64_t SymbolBody::template getGotPltVA<ELF64LE>() const;
template uint64_t SymbolBody::template getGotPltVA<ELF64BE>() const;

template uint32_t SymbolBody::template getPltVA<ELF32LE>() const;
template uint32_t SymbolBody::template getPltVA<ELF32BE>() const;
template uint64_t SymbolBody::template getPltVA<ELF64LE>() const;
template uint64_t SymbolBody::template getPltVA<ELF64BE>() const;

template uint32_t SymbolBody::template getSize<ELF32LE>() const;
template uint32_t SymbolBody::template getSize<ELF32BE>() const;
template uint64_t SymbolBody::template getSize<ELF64LE>() const;
template uint64_t SymbolBody::template getSize<ELF64BE>() const;

template int SymbolBody::compare<ELF32LE>(SymbolBody *Other);
template int SymbolBody::compare<ELF32BE>(SymbolBody *Other);
template int SymbolBody::compare<ELF64LE>(SymbolBody *Other);
template int SymbolBody::compare<ELF64BE>(SymbolBody *Other);

template class elf::UndefinedElf<ELF32LE>;
template class elf::UndefinedElf<ELF32BE>;
template class elf::UndefinedElf<ELF64LE>;
template class elf::UndefinedElf<ELF64BE>;

template class elf::DefinedSynthetic<ELF32LE>;
template class elf::DefinedSynthetic<ELF32BE>;
template class elf::DefinedSynthetic<ELF64LE>;
template class elf::DefinedSynthetic<ELF64BE>;
