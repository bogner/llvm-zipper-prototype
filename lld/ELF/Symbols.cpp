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
static typename ELFT::uint getSymVA(const SymbolBody &Body,
                                    typename ELFT::uint &Addend) {
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::uint uintX_t;

  switch (Body.kind()) {
  case SymbolBody::DefinedSyntheticKind: {
    auto &D = cast<DefinedSynthetic<ELFT>>(Body);
    return D.Section.getVA() + D.Value;
  }
  case SymbolBody::DefinedRegularKind: {
    auto &D = cast<DefinedRegular<ELFT>>(Body);
    InputSectionBase<ELFT> *SC = D.Section;

    // This is an absolute symbol.
    if (!SC)
      return D.Sym.st_value;

    const Elf_Sym &Sym = D.Sym;
    uintX_t Offset = Sym.st_value;
    if (Sym.getType() == STT_SECTION) {
      Offset += Addend;
      Addend = 0;
    }
    uintX_t VA = SC->OutSec->getVA() + SC->getOffset(Offset);
    if (Sym.getType() == STT_TLS)
      return VA - Out<ELFT>::TlsPhdr->p_vaddr;
    return VA;
  }
  case SymbolBody::DefinedCommonKind:
    return Out<ELFT>::Bss->getVA() + cast<DefinedCommon>(Body).OffsetInBss;
  case SymbolBody::SharedKind: {
    auto &SS = cast<SharedSymbol<ELFT>>(Body);
    if (!SS.NeedsCopyOrPltAddr)
      return 0;
    if (SS.IsFunc)
      return Body.getPltVA<ELFT>();
    return Out<ELFT>::Bss->getVA() + SS.OffsetInBss;
  }
  case SymbolBody::UndefinedElfKind:
  case SymbolBody::UndefinedKind:
    return 0;
  case SymbolBody::LazyKind:
    assert(Body.isUsedInRegularObj() && "lazy symbol reached writer");
    return 0;
  case SymbolBody::DefinedBitcodeKind:
    llvm_unreachable("should have been replaced");
  }
  llvm_unreachable("invalid symbol kind");
}

// Returns true if a symbol can be replaced at load-time by a symbol
// with the same name defined in other ELF executable or DSO.
bool SymbolBody::isPreemptible() const {
  if (isLocal())
    return false;

  if (isShared())
    return true;

  if (isUndefined()) {
    if (!isWeak())
      return true;

    // Ideally the static linker should see a definition for every symbol, but
    // shared object are normally allowed to have undefined references that the
    // static linker never sees a definition for.
    if (Config->Shared)
      return true;

    // Otherwise, just resolve to 0.
    return false;
  }

  if (!Config->Shared)
    return false;
  if (getVisibility() != STV_DEFAULT)
    return false;
  if (Config->Bsymbolic || (Config->BsymbolicFunctions && IsFunc))
    return false;
  return true;
}

template <class ELFT> bool SymbolBody::isGnuIfunc() const {
  if (auto *D = dyn_cast<DefinedElf<ELFT>>(this))
    return D->Sym.getType() == STT_GNU_IFUNC;
  return false;
}

template <class ELFT>
typename ELFT::uint SymbolBody::getVA(typename ELFT::uint Addend) const {
  typename ELFT::uint OutVA = getSymVA<ELFT>(*this, Addend);
  return OutVA + Addend;
}

template <class ELFT> typename ELFT::uint SymbolBody::getGotVA() const {
  return Out<ELFT>::Got->getVA() +
         (Out<ELFT>::Got->getMipsLocalEntriesNum() + GotIndex) *
             sizeof(typename ELFT::uint);
}

template <class ELFT> typename ELFT::uint SymbolBody::getGotPltVA() const {
  return Out<ELFT>::GotPlt->getVA() + GotPltIndex * sizeof(typename ELFT::uint);
}

template <class ELFT> typename ELFT::uint SymbolBody::getPltVA() const {
  return Out<ELFT>::Plt->getVA() + Target->PltZeroSize +
         PltIndex * Target->PltEntrySize;
}

template <class ELFT> typename ELFT::uint SymbolBody::getSize() const {
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

static int compareCommons(DefinedCommon *A, DefinedCommon *B) {
  if (Config->WarnCommon)
    warning("multiple common of " + A->getName());
  A->Alignment = B->Alignment = std::max(A->Alignment, B->Alignment);
  if (A->Size < B->Size)
    return -1;
  return 1;
}

// Returns 1, 0 or -1 if this symbol should take precedence
// over the Other, tie or lose, respectively.
template <class ELFT> int SymbolBody::compare(SymbolBody *Other) {
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
  if (!isDefined() || isShared() || isWeak())
    return 1;
  if (!isCommon() && !Other->isCommon())
    return 0;
  if (isCommon() && Other->isCommon())
    return compareCommons(cast<DefinedCommon>(this),
                          cast<DefinedCommon>(Other));
  if (Config->WarnCommon)
    warning("common " + this->getName() + " is overridden");
  return isCommon() ? -1 : 1;
}

Defined::Defined(Kind K, StringRef Name, bool IsWeak, bool IsLocal,
                 uint8_t Visibility, uint8_t Type)
    : SymbolBody(K, Name, IsWeak, IsLocal, Visibility, Type) {}

DefinedBitcode::DefinedBitcode(StringRef Name, bool IsWeak, uint8_t Visibility)
    : Defined(DefinedBitcodeKind, Name, IsWeak, false, Visibility,
              0 /* Type */) {}

bool DefinedBitcode::classof(const SymbolBody *S) {
  return S->kind() == DefinedBitcodeKind;
}

Undefined::Undefined(SymbolBody::Kind K, StringRef N, bool IsWeak,
                     uint8_t Visibility, uint8_t Type)
    : SymbolBody(K, N, IsWeak, false, Visibility, Type),
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
    : Defined(SymbolBody::DefinedSyntheticKind, N, false, false, Visibility,
              0 /* Type */),
      Value(Value), Section(Section) {}

DefinedCommon::DefinedCommon(StringRef N, uint64_t Size, uint64_t Alignment,
                             bool IsWeak, uint8_t Visibility)
    : Defined(SymbolBody::DefinedCommonKind, N, IsWeak, false, Visibility,
              0 /* Type */),
      Alignment(Alignment), Size(Size) {}

std::unique_ptr<InputFile> Lazy::getMember() {
  MemoryBufferRef MBRef = File->getMember(&Sym);

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (MBRef.getBuffer().empty())
    return std::unique_ptr<InputFile>(nullptr);
  return createObjectFile(MBRef, File->getName());
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

template bool SymbolBody::template isGnuIfunc<ELF32LE>() const;
template bool SymbolBody::template isGnuIfunc<ELF32BE>() const;
template bool SymbolBody::template isGnuIfunc<ELF64LE>() const;
template bool SymbolBody::template isGnuIfunc<ELF64BE>() const;

template uint32_t SymbolBody::template getVA<ELF32LE>(uint32_t) const;
template uint32_t SymbolBody::template getVA<ELF32BE>(uint32_t) const;
template uint64_t SymbolBody::template getVA<ELF64LE>(uint64_t) const;
template uint64_t SymbolBody::template getVA<ELF64BE>(uint64_t) const;

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
