//===- SymbolTable.cpp ----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "Driver.h"
#include "Error.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/LTO/LTOCodeGenerator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <utility>

using namespace llvm;

namespace lld {
namespace coff {

SymbolTable::SymbolTable() {
  addSymbol(new (Alloc) DefinedAbsolute("__ImageBase", Config->ImageBase));
}

void SymbolTable::addFile(std::unique_ptr<InputFile> FileP) {
  InputFile *File = FileP.get();
  Files.push_back(std::move(FileP));
  if (auto *F = dyn_cast<ArchiveFile>(File)) {
    ArchiveQueue.push_back(F);
    return;
  }
  ObjectQueue.push_back(File);
  if (auto *F = dyn_cast<ObjectFile>(File)) {
    ObjectFiles.push_back(F);
  } else if (auto *F = dyn_cast<BitcodeFile>(File)) {
    BitcodeFiles.push_back(F);
  } else {
    ImportFiles.push_back(cast<ImportFile>(File));
  }
}

std::error_code SymbolTable::step() {
  if (queueEmpty())
    return std::error_code();
  if (auto EC = readObjects())
    return EC;
  if (auto EC = readArchives())
    return EC;
  return std::error_code();
}

std::error_code SymbolTable::run() {
  while (!queueEmpty())
    if (auto EC = step())
      return EC;
  return std::error_code();
}

std::error_code SymbolTable::readArchives() {
  if (ArchiveQueue.empty())
    return std::error_code();

  // Add lazy symbols to the symbol table. Lazy symbols that conflict
  // with existing undefined symbols are accumulated in LazySyms.
  std::vector<Symbol *> LazySyms;
  for (ArchiveFile *File : ArchiveQueue) {
    if (Config->Verbose)
      llvm::outs() << "Reading " << File->getShortName() << "\n";
    if (auto EC = File->parse())
      return EC;
    for (Lazy *Sym : File->getLazySymbols())
      addLazy(Sym, &LazySyms);
  }
  ArchiveQueue.clear();

  // Add archive member files to ObjectQueue that should resolve
  // existing undefined symbols.
  for (Symbol *Sym : LazySyms)
    if (auto EC = addMemberFile(cast<Lazy>(Sym->Body)))
      return EC;
  return std::error_code();
}

std::error_code SymbolTable::readObjects() {
  if (ObjectQueue.empty())
    return std::error_code();

  // Add defined and undefined symbols to the symbol table.
  std::vector<StringRef> Directives;
  for (size_t I = 0; I < ObjectQueue.size(); ++I) {
    InputFile *File = ObjectQueue[I];
    if (Config->Verbose)
      llvm::outs() << "Reading " << File->getShortName() << "\n";
    if (auto EC = File->parse())
      return EC;
    // Adding symbols may add more files to ObjectQueue
    // (but not to ArchiveQueue).
    for (SymbolBody *Sym : File->getSymbols())
      if (Sym->isExternal())
        if (auto EC = addSymbol(Sym))
          return EC;
    StringRef S = File->getDirectives();
    if (!S.empty())
      Directives.push_back(S);
  }
  ObjectQueue.clear();

  // Parse directive sections. This may add files to
  // ArchiveQueue and ObjectQueue.
  for (StringRef S : Directives)
    if (auto EC = Driver->parseDirectives(S))
      return EC;
  return std::error_code();
}

bool SymbolTable::queueEmpty() {
  return ArchiveQueue.empty() && ObjectQueue.empty();
}

bool SymbolTable::reportRemainingUndefines() {
  bool Ret = false;
  for (auto &I : Symtab) {
    Symbol *Sym = I.second;
    auto *Undef = dyn_cast<Undefined>(Sym->Body);
    if (!Undef)
      continue;
    StringRef Name = Undef->getName();
    // A weak alias may have been resovled, so check for that. A weak alias
    // may be an weak alias to other symbol, so check recursively.
    for (Undefined *U = Undef->WeakAlias; U; U = U->WeakAlias) {
      if (auto *D = dyn_cast<Defined>(U->repl())) {
        Sym->Body = D;
        goto next;
      }
    }
    // If we can resolve a symbol by removing __imp_ prefix, do that.
    // This odd rule is for compatibility with MSVC linker.
    if (Name.startswith("__imp_")) {
      Symbol *Imp = find(Name.substr(strlen("__imp_")));
      if (Imp && isa<Defined>(Imp->Body)) {
        auto *D = cast<Defined>(Imp->Body);
        auto *S = new (Alloc) DefinedLocalImport(Name, D);
        LocalImportChunks.push_back(S->getChunk());
        Sym->Body = S;
        continue;
      }
    }
    llvm::errs() << "undefined symbol: " << Name << "\n";
    // Remaining undefined symbols are not fatal if /force is specified.
    // They are replaced with dummy defined symbols.
    if (Config->Force) {
      Sym->Body = new (Alloc) DefinedAbsolute(Name, 0);
      continue;
    }
    Ret = true;
    next:;
  }
  return Ret;
}

void SymbolTable::addLazy(Lazy *New, std::vector<Symbol *> *Accum) {
  Symbol *Sym = insert(New);
  if (Sym->Body == New)
    return;
  SymbolBody *Existing = Sym->Body;
  if (!isa<Undefined>(Existing))
    return;
  Sym->Body = New;
  New->setBackref(Sym);
  Accum->push_back(Sym);
}

std::error_code SymbolTable::addSymbol(SymbolBody *New) {
  // Find an existing symbol or create and insert a new one.
  assert(isa<Defined>(New) || isa<Undefined>(New));
  Symbol *Sym = insert(New);
  if (Sym->Body == New)
    return std::error_code();
  SymbolBody *Existing = Sym->Body;

  // If we have an undefined symbol and a lazy symbol,
  // let the lazy symbol to read a member file.
  if (auto *L = dyn_cast<Lazy>(Existing)) {
    // Undefined symbols with weak aliases need not to be resolved,
    // since they would be replaced with weak aliases if they remain
    // undefined.
    if (auto *U = dyn_cast<Undefined>(New))
      if (!U->WeakAlias)
        return addMemberFile(L);
    Sym->Body = New;
    return std::error_code();
  }

  // compare() returns -1, 0, or 1 if the lhs symbol is less preferable,
  // equivalent (conflicting), or more preferable, respectively.
  int Comp = Existing->compare(New);
  if (Comp == 0) {
    llvm::errs() << "duplicate symbol: " << Existing->getDebugName()
                 << " and " << New->getDebugName() << "\n";
    return make_error_code(LLDError::DuplicateSymbols);
  }
  if (Comp < 0)
    Sym->Body = New;
  return std::error_code();
}

Symbol *SymbolTable::insert(SymbolBody *New) {
  Symbol *&Sym = Symtab[New->getName()];
  if (Sym) {
    New->setBackref(Sym);
    return Sym;
  }
  Sym = new (Alloc) Symbol(New);
  New->setBackref(Sym);
  return Sym;
}

// Reads an archive member file pointed by a given symbol.
std::error_code SymbolTable::addMemberFile(Lazy *Body) {
  auto FileOrErr = Body->getMember();
  if (auto EC = FileOrErr.getError())
    return EC;
  std::unique_ptr<InputFile> File = std::move(FileOrErr.get());

  // getMember returns an empty buffer if the member was already
  // read from the library.
  if (!File)
    return std::error_code();
  if (Config->Verbose)
    llvm::outs() << "Loaded " << File->getShortName() << " for "
                 << Body->getName() << "\n";
  addFile(std::move(File));
  return std::error_code();
}

std::vector<Chunk *> SymbolTable::getChunks() {
  std::vector<Chunk *> Res;
  for (ObjectFile *File : ObjectFiles) {
    std::vector<Chunk *> &V = File->getChunks();
    Res.insert(Res.end(), V.begin(), V.end());
  }
  return Res;
}

Symbol *SymbolTable::find(StringRef Name) {
  auto It = Symtab.find(Name);
  if (It == Symtab.end())
    return nullptr;
  return It->second;
}

void SymbolTable::mangleMaybe(Undefined *U) {
  if (U->WeakAlias)
    return;
  if (!isa<Undefined>(U->repl()))
    return;

  // In Microsoft ABI, a non-member function name is mangled this way.
  std::string Prefix = ("?" + U->getName() + "@@Y").str();
  for (auto Pair : Symtab) {
    StringRef Name = Pair.first;
    if (!Name.startswith(Prefix))
      continue;
    U->WeakAlias = addUndefined(Name);
    return;
  }
}

Undefined *SymbolTable::addUndefined(StringRef Name) {
  auto *New = new (Alloc) Undefined(Name);
  addSymbol(New);
  if (auto *U = dyn_cast<Undefined>(New->repl()))
    return U;
  return New;
}

void SymbolTable::printMap(llvm::raw_ostream &OS) {
  for (ObjectFile *File : ObjectFiles) {
    OS << File->getShortName() << ":\n";
    for (SymbolBody *Body : File->getSymbols())
      if (auto *R = dyn_cast<DefinedRegular>(Body))
        if (R->isLive())
          OS << Twine::utohexstr(Config->ImageBase + R->getRVA())
             << " " << R->getName() << "\n";
  }
}

std::error_code SymbolTable::addCombinedLTOObject() {
  if (BitcodeFiles.empty())
    return std::error_code();

  // Create an object file and add it to the symbol table by replacing any
  // DefinedBitcode symbols with the definitions in the object file.
  LTOCodeGenerator CG;
  auto FileOrErr = createLTOObject(&CG);
  if (auto EC = FileOrErr.getError())
    return EC;
  ObjectFile *Obj = FileOrErr.get();

  for (SymbolBody *Body : Obj->getSymbols()) {
    if (!Body->isExternal())
      continue;
    // Find an existing Symbol. We should not see any new undefined symbols at
    // this point.
    StringRef Name = Body->getName();
    Symbol *Sym = insert(Body);
    if (Sym->Body == Body && !isa<Defined>(Body)) {
      llvm::errs() << "LTO: undefined symbol: " << Name << '\n';
      return make_error_code(LLDError::BrokenFile);
    }

    if (isa<DefinedBitcode>(Sym->Body)) {
      // The symbol should now be defined.
      if (!isa<Defined>(Body)) {
        llvm::errs() << "LTO: undefined symbol: " << Name << '\n';
        return make_error_code(LLDError::BrokenFile);
      }
      Sym->Body = Body;
      continue;
    }
    if (auto *L = dyn_cast<Lazy>(Sym->Body)) {
      // We may see new references to runtime library symbols such as __chkstk
      // here. These symbols must be wholly defined in non-bitcode files.
      if (auto EC = addMemberFile(L))
        return EC;
      continue;
    }
    SymbolBody *Existing = Sym->Body;
    int Comp = Existing->compare(Body);
    if (Comp == 0) {
      llvm::errs() << "LTO: unexpected duplicate symbol: " << Name << "\n";
      return make_error_code(LLDError::BrokenFile);
    }
    if (Comp < 0)
      Sym->Body = Body;
  }

  size_t NumBitcodeFiles = BitcodeFiles.size();
  if (auto EC = run())
    return EC;
  if (BitcodeFiles.size() != NumBitcodeFiles) {
    llvm::errs() << "LTO: late loaded symbol created new bitcode reference\n";
    return make_error_code(LLDError::BrokenFile);
  }

  // New runtime library symbol references may have created undefined references.
  if (reportRemainingUndefines())
    return make_error_code(LLDError::BrokenFile);
  return std::error_code();
}

// Combine and compile bitcode files and then return the result
// as a regular COFF object file.
ErrorOr<ObjectFile *> SymbolTable::createLTOObject(LTOCodeGenerator *CG) {
  // All symbols referenced by non-bitcode objects must be preserved.
  for (ObjectFile *File : ObjectFiles)
    for (SymbolBody *Body : File->getSymbols())
      if (auto *S = dyn_cast<DefinedBitcode>(Body->repl()))
        CG->addMustPreserveSymbol(S->getName());

  // Likewise for bitcode symbols which we initially resolved to non-bitcode.
  for (BitcodeFile *File : BitcodeFiles)
    for (SymbolBody *Body : File->getSymbols())
      if (isa<DefinedBitcode>(Body) && !isa<DefinedBitcode>(Body->repl()))
        CG->addMustPreserveSymbol(Body->getName());

  // Likewise for other symbols that must be preserved.
  for (Undefined *U : Config->GCRoot)
    if (isa<DefinedBitcode>(U->repl()))
      CG->addMustPreserveSymbol(U->getName());

  CG->setModule(BitcodeFiles[0]->releaseModule());
  for (unsigned I = 1, E = BitcodeFiles.size(); I != E; ++I)
    CG->addModule(BitcodeFiles[I]->getModule());

  std::string ErrMsg;
  LTOMB = CG->compile(false, false, false, ErrMsg); // take MB ownership
  if (!LTOMB) {
    llvm::errs() << ErrMsg << '\n';
    return make_error_code(LLDError::BrokenFile);
  }
  auto *Obj = new ObjectFile(LTOMB->getMemBufferRef());
  Files.emplace_back(Obj);
  ObjectFiles.push_back(Obj);
  if (auto EC = Obj->parse())
    return EC;
  return Obj;
}

} // namespace coff
} // namespace lld
