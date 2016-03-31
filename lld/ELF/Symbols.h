//===- Symbols.h ------------------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// All symbols are handled as SymbolBodies regardless of their types.
// This file defines various types of SymbolBodies.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_SYMBOLS_H
#define LLD_ELF_SYMBOLS_H

#include "InputSection.h"

#include "lld/Core/LLVM.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ELF.h"

namespace lld {
namespace elf {

class ArchiveFile;
class InputFile;
class SymbolBody;
template <class ELFT> class ObjectFile;
template <class ELFT> class OutputSection;
template <class ELFT> class OutputSectionBase;
template <class ELFT> class SharedFile;

// Returns a demangled C++ symbol name. If Name is not a mangled
// name or the system does not provide __cxa_demangle function,
// it returns the unmodified string.
std::string demangle(StringRef Name);

// A real symbol object, SymbolBody, is usually accessed indirectly
// through a Symbol. There's always one Symbol for each symbol name.
// The resolver updates SymbolBody pointers as it resolves symbols.
struct Symbol {
  SymbolBody *Body;
};

// The base class for real symbol classes.
class SymbolBody {
public:
  enum Kind {
    DefinedFirst,
    DefinedRegularKind = DefinedFirst,
    SharedKind,
    DefinedElfLast = SharedKind,
    DefinedCommonKind,
    DefinedBitcodeKind,
    DefinedSyntheticKind,
    DefinedLast = DefinedSyntheticKind,
    UndefinedElfKind,
    UndefinedKind,
    LazyKind
  };

  Kind kind() const { return static_cast<Kind>(SymbolKind); }

  bool isWeak() const { return IsWeak; }
  bool isUndefined() const {
    return SymbolKind == UndefinedKind || SymbolKind == UndefinedElfKind;
  }
  bool isDefined() const { return SymbolKind <= DefinedLast; }
  bool isCommon() const { return SymbolKind == DefinedCommonKind; }
  bool isLazy() const { return SymbolKind == LazyKind; }
  bool isShared() const { return SymbolKind == SharedKind; }
  bool isLocal() const { return IsLocal; }
  bool isUsedInRegularObj() const { return IsUsedInRegularObj; }
  bool isPreemptible() const;

  // Returns the symbol name.
  StringRef getName() const { return Name; }

  uint8_t getVisibility() const { return Visibility; }

  unsigned DynsymIndex = 0;
  uint32_t GlobalDynIndex = -1;
  uint32_t GotIndex = -1;
  uint32_t GotPltIndex = -1;
  uint32_t PltIndex = -1;
  uint32_t ThunkIndex = -1;
  bool hasGlobalDynIndex() { return GlobalDynIndex != uint32_t(-1); }
  bool isInGot() const { return GotIndex != -1U; }
  bool isInPlt() const { return PltIndex != -1U; }
  bool hasThunk() const { return ThunkIndex != -1U; }

  void setUsedInRegularObj() { IsUsedInRegularObj = true; }

  template <class ELFT>
  typename ELFT::uint getVA(typename ELFT::uint Addend = 0) const;

  template <class ELFT> typename ELFT::uint getGotVA() const;
  template <class ELFT> typename ELFT::uint getGotPltVA() const;
  template <class ELFT> typename ELFT::uint getPltVA() const;
  template <class ELFT> typename ELFT::uint getThunkVA() const;
  template <class ELFT> typename ELFT::uint getSize() const;

  // A SymbolBody has a backreference to a Symbol. Originally they are
  // doubly-linked. A backreference will never change. But the pointer
  // in the Symbol may be mutated by the resolver. If you have a
  // pointer P to a SymbolBody and are not sure whether the resolver
  // has chosen the object among other objects having the same name,
  // you can access P->Backref->Body to get the resolver's result.
  void setBackref(Symbol *P) { Backref = P; }
  SymbolBody &repl() { return Backref ? *Backref->Body : *this; }
  Symbol *getSymbol() const { return Backref; }

  // Decides which symbol should "win" in the symbol table, this or
  // the Other. Returns 1 if this wins, -1 if the Other wins, or 0 if
  // they are duplicate (conflicting) symbols.
  template <class ELFT> int compare(SymbolBody *Other);

protected:
  SymbolBody(Kind K, StringRef Name, bool IsWeak, bool IsLocal,
             uint8_t Visibility, uint8_t Type)
      : SymbolKind(K), IsWeak(IsWeak), IsLocal(IsLocal), Visibility(Visibility),
        MustBeInDynSym(false), NeedsCopyOrPltAddr(false), Name(Name) {
    IsFunc = Type == llvm::ELF::STT_FUNC;
    IsTls = Type == llvm::ELF::STT_TLS;
    IsGnuIFunc = Type == llvm::ELF::STT_GNU_IFUNC;
    IsUsedInRegularObj =
        K != SharedKind && K != LazyKind && K != DefinedBitcodeKind;
  }

  const unsigned SymbolKind : 8;
  unsigned IsWeak : 1;
  unsigned IsLocal : 1;
  unsigned Visibility : 2;

  // True if the symbol was used for linking and thus need to be
  // added to the output file's symbol table. It is usually true,
  // but if it is a shared symbol that were not referenced by anyone,
  // it can be false.
  unsigned IsUsedInRegularObj : 1;

public:
  // If true, the symbol is added to .dynsym symbol table.
  unsigned MustBeInDynSym : 1;

  // True if the linker has to generate a copy relocation for this shared
  // symbol or if the symbol should point to its plt entry.
  unsigned NeedsCopyOrPltAddr : 1;

  unsigned IsTls : 1;
  unsigned IsFunc : 1;
  unsigned IsGnuIFunc : 1;

protected:
  StringRef Name;
  Symbol *Backref = nullptr;
};

// The base class for any defined symbols.
class Defined : public SymbolBody {
public:
  Defined(Kind K, StringRef Name, bool IsWeak, bool IsLocal, uint8_t Visibility,
          uint8_t Type);
  static bool classof(const SymbolBody *S) { return S->isDefined(); }
};

// Any defined symbol from an ELF file.
template <class ELFT> class DefinedElf : public Defined {
protected:
  typedef typename ELFT::Sym Elf_Sym;

public:
  DefinedElf(Kind K, StringRef N, const Elf_Sym &Sym)
      : Defined(K, N, Sym.getBinding() == llvm::ELF::STB_WEAK,
                Sym.getBinding() == llvm::ELF::STB_LOCAL, Sym.getVisibility(),
                Sym.getType()),
        Sym(Sym) {}

  const Elf_Sym &Sym;
  static bool classof(const SymbolBody *S) {
    return S->kind() <= DefinedElfLast;
  }
};

class DefinedBitcode : public Defined {
public:
  DefinedBitcode(StringRef Name, bool IsWeak, uint8_t Visibility);
  static bool classof(const SymbolBody *S);
};

class DefinedCommon : public Defined {
public:
  DefinedCommon(StringRef N, uint64_t Size, uint64_t Alignment, bool IsWeak,
                uint8_t Visibility);

  static bool classof(const SymbolBody *S) {
    return S->kind() == SymbolBody::DefinedCommonKind;
  }

  // The output offset of this common symbol in the output bss. Computed by the
  // writer.
  uint64_t OffsetInBss;

  // The maximum alignment we have seen for this symbol.
  uint64_t Alignment;

  uint64_t Size;
};

// Regular defined symbols read from object file symbol tables.
template <class ELFT> class DefinedRegular : public DefinedElf<ELFT> {
  typedef typename ELFT::Sym Elf_Sym;

public:
  DefinedRegular(StringRef N, const Elf_Sym &Sym,
                 InputSectionBase<ELFT> *Section)
      : DefinedElf<ELFT>(SymbolBody::DefinedRegularKind, N, Sym),
        Section(Section ? Section->Repl : NullInputSection) {}

  static bool classof(const SymbolBody *S) {
    return S->kind() == SymbolBody::DefinedRegularKind;
  }

  // The input section this symbol belongs to. Notice that this is
  // a reference to a pointer. We are using two levels of indirections
  // because of ICF. If ICF decides two sections need to be merged, it
  // manipulates this Section pointers so that they point to the same
  // section. This is a bit tricky, so be careful to not be confused.
  // If this is null, the symbol is an absolute symbol.
  InputSectionBase<ELFT> *&Section;

private:
  static InputSectionBase<ELFT> *NullInputSection;
};

template <class ELFT>
InputSectionBase<ELFT> *DefinedRegular<ELFT>::NullInputSection;

// DefinedSynthetic is a class to represent linker-generated ELF symbols.
// The difference from the regular symbol is that DefinedSynthetic symbols
// don't belong to any input files or sections. Thus, its constructor
// takes an output section to calculate output VA, etc.
template <class ELFT> class DefinedSynthetic : public Defined {
public:
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::uint uintX_t;
  DefinedSynthetic(StringRef N, uintX_t Value, OutputSectionBase<ELFT> &Section,
                   uint8_t Visibility);

  static bool classof(const SymbolBody *S) {
    return S->kind() == SymbolBody::DefinedSyntheticKind;
  }

  // Special value designates that the symbol 'points'
  // to the end of the section.
  static const uintX_t SectionEnd = uintX_t(-1);

  uintX_t Value;
  const OutputSectionBase<ELFT> &Section;
};

// Undefined symbol.
class Undefined : public SymbolBody {
  typedef SymbolBody::Kind Kind;
  bool CanKeepUndefined;

protected:
  Undefined(Kind K, StringRef N, bool IsWeak, uint8_t Visibility, uint8_t Type);

public:
  Undefined(StringRef N, bool IsWeak, uint8_t Visibility,
            bool CanKeepUndefined);

  static bool classof(const SymbolBody *S) { return S->isUndefined(); }

  bool canKeepUndefined() const { return CanKeepUndefined; }
};

template <class ELFT> class UndefinedElf : public Undefined {
  typedef typename ELFT::Sym Elf_Sym;

public:
  UndefinedElf(StringRef N, const Elf_Sym &Sym);
  const Elf_Sym &Sym;

  static bool classof(const SymbolBody *S) {
    return S->kind() == SymbolBody::UndefinedElfKind;
  }
};

template <class ELFT> class SharedSymbol : public DefinedElf<ELFT> {
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::uint uintX_t;

public:
  static bool classof(const SymbolBody *S) {
    return S->kind() == SymbolBody::SharedKind;
  }

  SharedSymbol(SharedFile<ELFT> *F, StringRef Name, const Elf_Sym &Sym)
      : DefinedElf<ELFT>(SymbolBody::SharedKind, Name, Sym), File(F) {}

  SharedFile<ELFT> *File;

  // OffsetInBss is significant only when needsCopy() is true.
  uintX_t OffsetInBss = 0;

  bool needsCopy() const { return this->NeedsCopyOrPltAddr && !this->IsFunc; }
};

// This class represents a symbol defined in an archive file. It is
// created from an archive file header, and it knows how to load an
// object file from an archive to replace itself with a defined
// symbol. If the resolver finds both Undefined and Lazy for
// the same name, it will ask the Lazy to load a file.
class Lazy : public SymbolBody {
public:
  Lazy(ArchiveFile *F, const llvm::object::Archive::Symbol S)
      : SymbolBody(LazyKind, S.getName(), false, false, llvm::ELF::STV_DEFAULT,
                   /* Type */ 0),
        File(F), Sym(S) {}

  static bool classof(const SymbolBody *S) { return S->kind() == LazyKind; }

  // Returns an object file for this symbol, or a nullptr if the file
  // was already returned.
  std::unique_ptr<InputFile> getMember();

  void setWeak() { IsWeak = true; }

private:
  ArchiveFile *File;
  const llvm::object::Archive::Symbol Sym;
};

// Some linker-generated symbols need to be created as
// DefinedRegular symbols, so they need Elf_Sym symbols.
// Here we allocate such Elf_Sym symbols statically.
template <class ELFT> struct ElfSym {
  typedef typename ELFT::Sym Elf_Sym;

  // Used to represent an undefined symbol which we don't want to add to the
  // output file's symbol table. It has weak binding and can be substituted.
  static Elf_Sym Ignored;

  // The content for _etext and etext symbols.
  static Elf_Sym Etext;

  // The content for _edata and edata symbols.
  static Elf_Sym Edata;

  // The content for _end and end symbols.
  static Elf_Sym End;

  // The content for _gp symbol for MIPS target.
  static Elf_Sym MipsGp;

  // __rel_iplt_start/__rel_iplt_end for signaling
  // where R_[*]_IRELATIVE relocations do live.
  static Elf_Sym RelaIpltStart;
  static Elf_Sym RelaIpltEnd;
};

template <class ELFT> typename ELFT::Sym ElfSym<ELFT>::Ignored;
template <class ELFT> typename ELFT::Sym ElfSym<ELFT>::Etext;
template <class ELFT> typename ELFT::Sym ElfSym<ELFT>::Edata;
template <class ELFT> typename ELFT::Sym ElfSym<ELFT>::End;
template <class ELFT> typename ELFT::Sym ElfSym<ELFT>::MipsGp;
template <class ELFT> typename ELFT::Sym ElfSym<ELFT>::RelaIpltStart;
template <class ELFT> typename ELFT::Sym ElfSym<ELFT>::RelaIpltEnd;

} // namespace elf
} // namespace lld

#endif
