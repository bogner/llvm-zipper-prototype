//===- SymbolTable.cpp ----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Symbol table is a bag of all known symbols. We put all symbols of
// all input files to the symbol table. The symbol table is basically
// a hash table with the logic to resolve symbol name conflicts using
// the symbol types.
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "Config.h"
#include "Error.h"
#include "Symbols.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/StringSaver.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

using namespace lld;
using namespace lld::elf;

// All input object files must be for the same architecture
// (e.g. it does not make sense to link x86 object files with
// MIPS object files.) This function checks for that error.
template <class ELFT> static bool isCompatible(InputFile *FileP) {
  auto *F = dyn_cast<ELFFileBase<ELFT>>(FileP);
  if (!F)
    return true;
  if (F->getELFKind() == Config->EKind && F->getEMachine() == Config->EMachine)
    return true;
  StringRef A = F->getName();
  StringRef B = Config->Emulation;
  if (B.empty())
    B = Config->FirstElf->getName();
  error(A + " is incompatible with " + B);
  return false;
}

// Returns "(internal)", "foo.a(bar.o)" or "baz.o".
static std::string getFilename(InputFile *F) {
  if (!F)
    return "(internal)";
  if (!F->ArchiveName.empty())
    return (F->ArchiveName + "(" + F->getName() + ")").str();
  return F->getName();
}

// Add symbols in File to the symbol table.
template <class ELFT>
void SymbolTable<ELFT>::addFile(std::unique_ptr<InputFile> File) {
  InputFile *FileP = File.get();
  if (!isCompatible<ELFT>(FileP))
    return;

  // .a file
  if (auto *F = dyn_cast<ArchiveFile>(FileP)) {
    ArchiveFiles.emplace_back(cast<ArchiveFile>(File.release()));
    F->parse();
    for (Lazy &Sym : F->getLazySymbols())
      addLazy(&Sym);
    return;
  }

  // Lazy object file
  if (auto *F = dyn_cast<LazyObjectFile>(FileP)) {
    LazyObjectFiles.emplace_back(cast<LazyObjectFile>(File.release()));
    F->parse();
    for (Lazy &Sym : F->getLazySymbols())
      addLazy(&Sym);
    return;
  }

  if (Config->Trace)
    llvm::outs() << getFilename(FileP) << "\n";

  // .so file
  if (auto *F = dyn_cast<SharedFile<ELFT>>(FileP)) {
    // DSOs are uniquified not by filename but by soname.
    F->parseSoName();
    if (!SoNames.insert(F->getSoName()).second)
      return;

    SharedFiles.emplace_back(cast<SharedFile<ELFT>>(File.release()));
    F->parseRest();
    for (SharedSymbol<ELFT> &B : F->getSharedSymbols())
      resolve(&B);
    return;
  }

  // LLVM bitcode file
  if (auto *F = dyn_cast<BitcodeFile>(FileP)) {
    BitcodeFiles.emplace_back(cast<BitcodeFile>(File.release()));
    F->parse(ComdatGroups);
    for (SymbolBody *B : F->getSymbols())
      if (B)
        resolve(B);
    return;
  }

  // Regular object file
  auto *F = cast<ObjectFile<ELFT>>(FileP);
  ObjectFiles.emplace_back(cast<ObjectFile<ELFT>>(File.release()));
  F->parse(ComdatGroups);
  for (SymbolBody *B : F->getNonLocalSymbols())
    resolve(B);
}

// This function is where all the optimizations of link-time
// optimization happens. When LTO is in use, some input files are
// not in native object file format but in the LLVM bitcode format.
// This function compiles bitcode files into a few big native files
// using LLVM functions and replaces bitcode symbols with the results.
// Because all bitcode files that consist of a program are passed
// to the compiler at once, it can do whole-program optimization.
template <class ELFT> void SymbolTable<ELFT>::addCombinedLtoObject() {
  if (BitcodeFiles.empty())
    return;

  // Compile bitcode files.
  Lto.reset(new BitcodeCompiler);
  for (const std::unique_ptr<BitcodeFile> &F : BitcodeFiles)
    Lto->add(*F);
  std::vector<std::unique_ptr<InputFile>> IFs = Lto->compile();

  // Replace bitcode symbols.
  for (auto &IF : IFs) {
    ObjectFile<ELFT> *Obj = cast<ObjectFile<ELFT>>(IF.release());

    llvm::DenseSet<StringRef> DummyGroups;
    Obj->parse(DummyGroups);
    for (SymbolBody *Body : Obj->getNonLocalSymbols()) {
      Symbol *Sym = insert(Body);
      if (!Sym->Body->isUndefined() && Body->isUndefined())
        continue;
      Sym->Body = Body;
    }
    ObjectFiles.emplace_back(Obj);
  }
}

// Add an undefined symbol.
template <class ELFT>
SymbolBody *SymbolTable<ELFT>::addUndefined(StringRef Name) {
  auto *Sym = new (Alloc) Undefined(Name, STB_GLOBAL, STV_DEFAULT, /*Type*/ 0,
                                    /*IsBitcode*/ false);
  resolve(Sym);
  return Sym;
}

template <class ELFT>
DefinedRegular<ELFT> *SymbolTable<ELFT>::addAbsolute(StringRef Name,
                                                     uint8_t Visibility) {
  // Pass nullptr because absolute symbols have no corresponding input sections.
  auto *Sym = new (Alloc) DefinedRegular<ELFT>(Name, STB_GLOBAL, Visibility);
  resolve(Sym);
  return Sym;
}

template <class ELFT>
SymbolBody *SymbolTable<ELFT>::addSynthetic(StringRef Name,
                                            OutputSectionBase<ELFT> &Sec,
                                            uintX_t Val) {
  auto *Sym = new (Alloc) DefinedSynthetic<ELFT>(Name, Val, Sec);
  resolve(Sym);
  return Sym;
}

// Add Name as an "ignored" symbol. An ignored symbol is a regular
// linker-synthesized defined symbol, but is only defined if needed.
template <class ELFT>
DefinedRegular<ELFT> *SymbolTable<ELFT>::addIgnored(StringRef Name,
                                                    uint8_t Visibility) {
  if (!find(Name))
    return nullptr;
  return addAbsolute(Name, Visibility);
}

// Rename SYM as __wrap_SYM. The original symbol is preserved as __real_SYM.
// Used to implement --wrap.
template <class ELFT> void SymbolTable<ELFT>::wrap(StringRef Name) {
  SymbolBody *B = find(Name);
  if (!B)
    return;
  StringSaver Saver(Alloc);
  Symbol *Sym = B->Backref;
  Symbol *Real = addUndefined(Saver.save("__real_" + Name))->Backref;
  Symbol *Wrap = addUndefined(Saver.save("__wrap_" + Name))->Backref;
  Real->Body = Sym->Body;
  Sym->Body = Wrap->Body;
}

// Returns a file from which symbol B was created.
// If B does not belong to any file, returns a nullptr.
// This function is slow, but it's okay as it is used only for error messages.
template <class ELFT> InputFile *SymbolTable<ELFT>::findFile(SymbolBody *B) {
  for (const std::unique_ptr<ObjectFile<ELFT>> &F : ObjectFiles) {
    ArrayRef<SymbolBody *> Syms = F->getSymbols();
    if (std::find(Syms.begin(), Syms.end(), B) != Syms.end())
      return F.get();
  }
  for (const std::unique_ptr<BitcodeFile> &F : BitcodeFiles) {
    ArrayRef<SymbolBody *> Syms = F->getSymbols();
    if (std::find(Syms.begin(), Syms.end(), B) != Syms.end())
      return F.get();
  }
  return nullptr;
}

// Construct a string in the form of "Sym in File1 and File2".
// Used to construct an error message.
template <class ELFT>
std::string SymbolTable<ELFT>::conflictMsg(SymbolBody *Old, SymbolBody *New) {
  InputFile *F1 = findFile(Old);
  InputFile *F2 = findFile(New);
  StringRef Sym = Old->getName();
  return demangle(Sym) + " in " + getFilename(F1) + " and " + getFilename(F2);
}

// This function resolves conflicts if there's an existing symbol with
// the same name. Decisions are made based on symbol type.
template <class ELFT> void SymbolTable<ELFT>::resolve(SymbolBody *New) {
  Symbol *Sym = insert(New);
  if (Sym->Body == New)
    return;

  SymbolBody *Existing = Sym->Body;

  if (auto *L = dyn_cast<Lazy>(Existing)) {
    Sym->Binding = New->Binding;
    if (New->isUndefined()) {
      addMemberFile(New, L);
      return;
    }
    // Found a definition for something also in an archive.
    // Ignore the archive definition.
    Sym->Body = New;
    return;
  }

  if (New->isTls() != Existing->isTls()) {
    error("TLS attribute mismatch for symbol: " + conflictMsg(Existing, New));
    return;
  }

  // compare() returns -1, 0, or 1 if the lhs symbol is less preferable,
  // equivalent (conflicting), or more preferable, respectively.
  int Comp = Existing->compare(New);
  if (Comp == 0) {
    std::string S = "duplicate symbol: " + conflictMsg(Existing, New);
    if (Config->AllowMultipleDefinition)
      warning(S);
    else
      error(S);
    return;
  }
  if (Comp < 0) {
    Sym->Body = New;
    if (!New->isShared())
      Sym->Binding = New->Binding;
  }
}

static uint8_t getMinVisibility(uint8_t VA, uint8_t VB) {
  if (VA == STV_DEFAULT)
    return VB;
  if (VB == STV_DEFAULT)
    return VA;
  return std::min(VA, VB);
}

static bool shouldExport(SymbolBody *B) {
  if (Config->Shared || Config->ExportDynamic) {
    // Export most symbols except for those that do not need to be exported.
    return !B->CanOmitFromDynSym;
  }
  // Make sure we preempt DSO symbols with default visibility.
  return B->isShared() && B->getVisibility() == STV_DEFAULT;
}

// Find an existing symbol or create and insert a new one.
template <class ELFT> Symbol *SymbolTable<ELFT>::insert(SymbolBody *New) {
  StringRef Name = New->getName();
  unsigned NumSyms = SymVector.size();
  auto P = Symtab.insert(std::make_pair(Name, NumSyms));
  Symbol *Sym;
  if (P.second) {
    Sym = new (Alloc) Symbol;
    Sym->Body = New;
    Sym->Binding = New->isShared() ? (uint8_t)STB_GLOBAL : New->Binding;
    Sym->Visibility = STV_DEFAULT;
    Sym->IsUsedInRegularObj = false;
    Sym->ExportDynamic = false;
    Sym->VersionScriptGlobal = !Config->VersionScript;
    SymVector.push_back(Sym);
  } else {
    Sym = SymVector[P.first->second];
  }
  New->Backref = Sym;

  // Merge in the new symbol's visibility. DSO symbols do not affect visibility
  // in the output.
  if (!New->isShared())
    Sym->Visibility = getMinVisibility(Sym->Visibility, New->getVisibility());
  Sym->ExportDynamic = Sym->ExportDynamic || shouldExport(New);
  SymbolBody::Kind K = New->kind();
  if (K == SymbolBody::DefinedRegularKind ||
      K == SymbolBody::DefinedCommonKind ||
      K == SymbolBody::DefinedSyntheticKind ||
      (K == SymbolBody::UndefinedKind && !New->IsUndefinedBitcode))
    Sym->IsUsedInRegularObj = true;
  return Sym;
}

template <class ELFT> SymbolBody *SymbolTable<ELFT>::find(StringRef Name) {
  auto It = Symtab.find(Name);
  if (It == Symtab.end())
    return nullptr;
  return SymVector[It->second]->Body;
}

template <class ELFT> void SymbolTable<ELFT>::addLazy(Lazy *L) {
  Symbol *Sym = insert(L);
  SymbolBody *Cur = Sym->Body;
  if (Cur == L)
    return;
  if (Cur->isUndefined()) {
    Sym->Body = L;
    addMemberFile(Cur, L);
  }
}

template <class ELFT>
void SymbolTable<ELFT>::addMemberFile(SymbolBody *Undef, Lazy *L) {
  // Weak undefined symbols should not fetch members from archives.
  // If we were to keep old symbol we would not know that an archive member was
  // available if a strong undefined symbol shows up afterwards in the link.
  // If a strong undefined symbol never shows up, this lazy symbol will
  // get to the end of the link and must be treated as the weak undefined one.
  // We already marked this symbol as used when we added it to the symbol table,
  // but we also need to preserve its binding and type.
  if (Undef->isWeak()) {
    // FIXME: Consider moving these members to Symbol.
    L->Type = Undef->Type;
    return;
  }

  // Fetch a member file that has the definition for L.
  // getMember returns nullptr if the member was already read from the library.
  if (std::unique_ptr<InputFile> File = L->getFile())
    addFile(std::move(File));
}

// Process undefined (-u) flags by loading lazy symbols named by those flags.
template <class ELFT>
void SymbolTable<ELFT>::scanUndefinedFlags() {
  for (StringRef S : Config->Undefined)
    if (SymbolBody *Sym = find(S))
      if (auto *L = dyn_cast<Lazy>(Sym))
        if (std::unique_ptr<InputFile> File = L->getFile())
          addFile(std::move(File));
}

// This function takes care of the case in which shared libraries depend on
// the user program (not the other way, which is usual). Shared libraries
// may have undefined symbols, expecting that the user program provides
// the definitions for them. An example is BSD's __progname symbol.
// We need to put such symbols to the main program's .dynsym so that
// shared libraries can find them.
// Except this, we ignore undefined symbols in DSOs.
template <class ELFT> void SymbolTable<ELFT>::scanShlibUndefined() {
  for (std::unique_ptr<SharedFile<ELFT>> &File : SharedFiles)
    for (StringRef U : File->getUndefinedSymbols())
      if (SymbolBody *Sym = find(U))
        if (Sym->isDefined())
          Sym->Backref->ExportDynamic = true;
}

// This function process the dynamic list option by marking all the symbols
// to be exported in the dynamic table.
template <class ELFT> void SymbolTable<ELFT>::scanDynamicList() {
  for (StringRef S : Config->DynamicList)
    if (SymbolBody *B = find(S))
      B->Backref->ExportDynamic = true;
}

// This function processes the --version-script option by marking all global
// symbols with the VersionScriptGlobal flag, which acts as a filter on the
// dynamic symbol table.
template <class ELFT> void SymbolTable<ELFT>::scanVersionScript() {
  for (StringRef S : Config->VersionScriptGlobals)
    if (SymbolBody *B = find(S))
      B->Backref->VersionScriptGlobal = true;
}

template class elf::SymbolTable<ELF32LE>;
template class elf::SymbolTable<ELF32BE>;
template class elf::SymbolTable<ELF64LE>;
template class elf::SymbolTable<ELF64BE>;
