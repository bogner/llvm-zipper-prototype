//===- SymbolTable.cpp ----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "Config.h"
#include "Error.h"
#include "Symbols.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

using namespace lld;
using namespace lld::elf2;

template <class ELFT> SymbolTable<ELFT>::SymbolTable() {}

template <class ELFT> bool SymbolTable<ELFT>::shouldUseRela() const {
  ELFKind K = getFirstELF()->getELFKind();
  return K == ELF64LEKind || K == ELF64BEKind;
}

template <class ELFT>
void SymbolTable<ELFT>::addFile(std::unique_ptr<InputFile> File) {

  if (auto *E = dyn_cast<ELFFileBase>(File.get())) {
    if (E->getELFKind() != Config->ElfKind ||
        E->getEMachine() != Config->EMachine) {
      StringRef A = E->getName();
      StringRef B = Config->Emulation;
      if (B.empty())
        B = Config->FirstElf->getName();
      error(A + " is incompatible with " + B);
    }
  }

  if (auto *AF = dyn_cast<ArchiveFile>(File.get())) {
    ArchiveFiles.emplace_back(std::move(File));
    AF->parse();
    for (Lazy &Sym : AF->getLazySymbols())
      addLazy(&Sym);
    return;
  }
  if (auto *S = dyn_cast<SharedFileBase>(File.get())) {
    S->parseSoName();
    if (!IncludedSoNames.insert(S->getSoName()).second)
      return;
    S->parse();
  } else {
    cast<ObjectFileBase>(File.get())->parse(Comdats);
  }
  addELFFile(cast<ELFFileBase>(File.release()));
}

template <class ELFT>
SymbolBody *SymbolTable<ELFT>::addUndefined(StringRef Name) {
  auto *Sym = new (Alloc) Undefined<ELFT>(Name, Undefined<ELFT>::Required);
  resolve(Sym);
  return Sym;
}

template <class ELFT>
SymbolBody *SymbolTable<ELFT>::addUndefinedOpt(StringRef Name) {
  auto *Sym = new (Alloc) Undefined<ELFT>(Name, Undefined<ELFT>::Optional);
  resolve(Sym);
  return Sym;
}

template <class ELFT>
void SymbolTable<ELFT>::addSyntheticSym(StringRef Name,
                                        OutputSection<ELFT> &Section,
                                        typename ELFFile<ELFT>::uintX_t Value) {
  typedef typename DefinedSynthetic<ELFT>::Elf_Sym Elf_Sym;
  auto ESym = new (Alloc) Elf_Sym;
  memset(ESym, 0, sizeof(Elf_Sym));
  ESym->st_value = Value;
  auto Sym = new (Alloc) DefinedSynthetic<ELFT>(Name, *ESym, Section);
  resolve(Sym);
}

template <class ELFT> void SymbolTable<ELFT>::addIgnoredSym(StringRef Name) {
  auto Sym = new (Alloc)
      DefinedAbsolute<ELFT>(Name, DefinedAbsolute<ELFT>::IgnoreUndef);
  resolve(Sym);
}

template <class ELFT> void SymbolTable<ELFT>::addELFFile(ELFFileBase *File) {
  if (auto *O = dyn_cast<ObjectFile<ELFT>>(File))
    ObjectFiles.emplace_back(O);
  else if (auto *S = dyn_cast<SharedFile<ELFT>>(File))
    SharedFiles.emplace_back(S);

  if (auto *O = dyn_cast<ObjectFileBase>(File)) {
    for (SymbolBody *Body : O->getSymbols())
      resolve(Body);
  }

  if (auto *S = dyn_cast<SharedFile<ELFT>>(File)) {
    for (SharedSymbol<ELFT> &Body : S->getSharedSymbols())
      resolve(&Body);
  }
}

template <class ELFT>
void SymbolTable<ELFT>::reportConflict(const Twine &Message,
                                       const SymbolBody &Old,
                                       const SymbolBody &New, bool Warning) {
  typedef typename ELFFile<ELFT>::Elf_Sym Elf_Sym;
  typedef typename ELFFile<ELFT>::Elf_Sym_Range Elf_Sym_Range;

  const Elf_Sym &OldE = cast<ELFSymbolBody<ELFT>>(Old).Sym;
  const Elf_Sym &NewE = cast<ELFSymbolBody<ELFT>>(New).Sym;
  ELFFileBase *OldFile = nullptr;
  ELFFileBase *NewFile = nullptr;

  for (const std::unique_ptr<ObjectFile<ELFT>> &File : ObjectFiles) {
    Elf_Sym_Range Syms = File->getObj().symbols(File->getSymbolTable());
    if (&OldE > Syms.begin() && &OldE < Syms.end())
      OldFile = File.get();
    if (&NewE > Syms.begin() && &NewE < Syms.end())
      NewFile = File.get();
  }

  std::string Msg = (Message + ": " + Old.getName() + " in " +
                     OldFile->getName() + " and " + NewFile->getName())
                        .str();
  if (Warning)
    warning(Msg);
  else
    error(Msg);
}

// This function resolves conflicts if there's an existing symbol with
// the same name. Decisions are made based on symbol type.
template <class ELFT> void SymbolTable<ELFT>::resolve(SymbolBody *New) {
  Symbol *Sym = insert(New);
  if (Sym->Body == New)
    return;

  SymbolBody *Existing = Sym->Body;

  if (Lazy *L = dyn_cast<Lazy>(Existing)) {
    if (New->isUndefined()) {
      if (New->isWeak()) {
        // See the explanation in SymbolTable::addLazy
        L->setUsedInRegularObj();
        L->setWeak();
        return;
      }
      addMemberFile(L);
      return;
    }

    // Found a definition for something also in an archive. Ignore the archive
    // definition.
    Sym->Body = New;
    return;
  }

  if (New->isTLS() != Existing->isTLS())
    reportConflict("TLS attribute mismatch for symbol", *Existing, *New, false);

  // compare() returns -1, 0, or 1 if the lhs symbol is less preferable,
  // equivalent (conflicting), or more preferable, respectively.
  int comp = Existing->compare<ELFT>(New);
  if (comp < 0)
    Sym->Body = New;
  else if (comp == 0)
    reportConflict("duplicate symbol", *Existing, *New,
                   Config->AllowMultipleDefinition);
}

template <class ELFT> Symbol *SymbolTable<ELFT>::insert(SymbolBody *New) {
  // Find an existing Symbol or create and insert a new one.
  StringRef Name = New->getName();
  Symbol *&Sym = Symtab[Name];
  if (!Sym) {
    Sym = new (Alloc) Symbol(New);
    New->setBackref(Sym);
    return Sym;
  }
  New->setBackref(Sym);
  return Sym;
}

template <class ELFT> void SymbolTable<ELFT>::addLazy(Lazy *New) {
  Symbol *Sym = insert(New);
  if (Sym->Body == New)
    return;
  SymbolBody *Existing = Sym->Body;
  if (Existing->isDefined() || Existing->isLazy())
    return;
  Sym->Body = New;
  assert(Existing->isUndefined() && "Unexpected symbol kind.");

  // Weak undefined symbols should not fetch members from archives.
  // If we were to keep old symbol we would not know that an archive member was
  // available if a strong undefined symbol shows up afterwards in the link.
  // If a strong undefined symbol never shows up, this lazy symbol will
  // get to the end of the link and must be treated as the weak undefined one.
  // We set UsedInRegularObj in a similar way to what is done with shared
  // symbols and mark it as weak to reduce how many special cases are needed.
  if (Existing->isWeak()) {
    New->setUsedInRegularObj();
    New->setWeak();
    return;
  }
  addMemberFile(New);
}

template <class ELFT> void SymbolTable<ELFT>::addMemberFile(Lazy *Body) {
  std::unique_ptr<InputFile> File = Body->getMember();

  // getMember returns nullptr if the member was already read from the library.
  if (!File)
    return;

  addFile(std::move(File));
}

template class lld::elf2::SymbolTable<ELF32LE>;
template class lld::elf2::SymbolTable<ELF32BE>;
template class lld::elf2::SymbolTable<ELF64LE>;
template class lld::elf2::SymbolTable<ELF64BE>;
