//===- InputFiles.cpp -----------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Chunks.h"
#include "Error.h"
#include "InputFiles.h"
#include "Writer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/LTO/LTOModule.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"
#include <mutex>

using namespace llvm::object;
using namespace llvm::support::endian;
using llvm::COFF::ImportHeader;
using llvm::COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE;
using llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
using llvm::COFF::IMAGE_FILE_MACHINE_UNKNOWN;
using llvm::RoundUpToAlignment;
using llvm::sys::fs::identify_magic;
using llvm::sys::fs::file_magic;

namespace lld {
namespace coff {

int InputFile::NextIndex = 0;

// Returns the last element of a path, which is supposed to be a filename.
static StringRef getBasename(StringRef Path) {
  size_t Pos = Path.find_last_of("\\/");
  if (Pos == StringRef::npos)
    return Path;
  return Path.substr(Pos + 1);
}

// Returns a string in the format of "foo.obj" or "foo.obj(bar.lib)".
std::string InputFile::getShortName() {
  if (ParentName == "")
    return getName().lower();
  std::string Res = (getBasename(ParentName) + "(" +
                     getBasename(getName()) + ")").str();
  return StringRef(Res).lower();
}

std::error_code ArchiveFile::parse() {
  // Parse a MemoryBufferRef as an archive file.
  auto ArchiveOrErr = Archive::create(MB);
  if (auto EC = ArchiveOrErr.getError())
    return EC;
  File = std::move(ArchiveOrErr.get());

  // Allocate a buffer for Lazy objects.
  size_t NumSyms = File->getNumberOfSymbols();
  size_t BufSize = NumSyms * sizeof(Lazy);
  Lazy *Buf = (Lazy *)Alloc.Allocate(BufSize, llvm::alignOf<Lazy>());
  LazySymbols.reserve(NumSyms);

  // Read the symbol table to construct Lazy objects.
  uint32_t I = 0;
  for (const Archive::Symbol &Sym : File->symbols()) {
    auto *B = new (&Buf[I++]) Lazy(this, Sym);
    // Skip special symbol exists in import library files.
    if (B->getName() != "__NULL_IMPORT_DESCRIPTOR")
      LazySymbols.push_back(B);
  }

  // Seen is a map from member files to boolean values. Initially
  // all members are mapped to false, which indicates all these files
  // are not read yet.
  for (const Archive::Child &Child : File->children())
    Seen[Child.getBuffer().data()].clear();
  return std::error_code();
}

// Returns a buffer pointing to a member file containing a given symbol.
// This function is thread-safe.
ErrorOr<MemoryBufferRef> ArchiveFile::getMember(const Archive::Symbol *Sym) {
  auto ItOrErr = Sym->getMember();
  if (auto EC = ItOrErr.getError())
    return EC;
  Archive::child_iterator It = ItOrErr.get();

  // Return an empty buffer if we have already returned the same buffer.
  const char *StartAddr = It->getBuffer().data();
  if (Seen[StartAddr].test_and_set())
    return MemoryBufferRef();
  return It->getMemoryBufferRef();
}

std::error_code ObjectFile::parse() {
  // Parse a memory buffer as a COFF file.
  auto BinOrErr = createBinary(MB);
  if (auto EC = BinOrErr.getError())
    return EC;
  std::unique_ptr<Binary> Bin = std::move(BinOrErr.get());

  if (auto *Obj = dyn_cast<COFFObjectFile>(Bin.get())) {
    Bin.release();
    COFFObj.reset(Obj);
  } else {
    llvm::errs() << getName() << " is not a COFF file.\n";
    return make_error_code(LLDError::InvalidFile);
  }

  // Read section and symbol tables.
  if (auto EC = initializeChunks())
    return EC;
  return initializeSymbols();
}

std::error_code ObjectFile::initializeChunks() {
  uint32_t NumSections = COFFObj->getNumberOfSections();
  Chunks.reserve(NumSections);
  SparseChunks.resize(NumSections + 1);
  for (uint32_t I = 1; I < NumSections + 1; ++I) {
    const coff_section *Sec;
    StringRef Name;
    if (auto EC = COFFObj->getSection(I, Sec)) {
      llvm::errs() << "getSection failed: " << Name << ": "
                   << EC.message() << "\n";
      return make_error_code(LLDError::BrokenFile);
    }
    if (auto EC = COFFObj->getSectionName(Sec, Name)) {
      llvm::errs() << "getSectionName failed: " << Name << ": "
                   << EC.message() << "\n";
      return make_error_code(LLDError::BrokenFile);
    }
    if (Name == ".drectve") {
      ArrayRef<uint8_t> Data;
      COFFObj->getSectionContents(Sec, Data);
      Directives = std::string((const char *)Data.data(), Data.size());
      continue;
    }
    // We want to preserve DWARF debug sections only when /debug is on.
    if (!Config->Debug && Name.startswith(".debug"))
      continue;
    if (Sec->Characteristics & llvm::COFF::IMAGE_SCN_LNK_REMOVE)
      continue;
    auto *C = new (Alloc) SectionChunk(this, Sec);
    Chunks.push_back(C);
    SparseChunks[I] = C;
  }
  return std::error_code();
}

std::error_code ObjectFile::initializeSymbols() {
  uint32_t NumSymbols = COFFObj->getNumberOfSymbols();
  SymbolBodies.reserve(NumSymbols);
  SparseSymbolBodies.resize(NumSymbols);
  int32_t LastSectionNumber = 0;
  for (uint32_t I = 0; I < NumSymbols; ++I) {
    // Get a COFFSymbolRef object.
    auto SymOrErr = COFFObj->getSymbol(I);
    if (auto EC = SymOrErr.getError()) {
      llvm::errs() << "broken object file: " << getName() << ": "
                   << EC.message() << "\n";
      return make_error_code(LLDError::BrokenFile);
    }
    COFFSymbolRef Sym = SymOrErr.get();

    const void *AuxP = nullptr;
    if (Sym.getNumberOfAuxSymbols())
      AuxP = COFFObj->getSymbol(I + 1)->getRawPtr();
    bool IsFirst = (LastSectionNumber != Sym.getSectionNumber());

    SymbolBody *Body = nullptr;
    if (Sym.isUndefined()) {
      Body = createUndefined(Sym);
    } else if (Sym.isWeakExternal()) {
      Body = createWeakExternal(Sym, AuxP);
    } else {
      Body = createDefined(Sym, AuxP, IsFirst);
    }
    if (Body) {
      SymbolBodies.push_back(Body);
      SparseSymbolBodies[I] = Body;
    }
    I += Sym.getNumberOfAuxSymbols();
    LastSectionNumber = Sym.getSectionNumber();
  }
  return std::error_code();
}

Undefined *ObjectFile::createUndefined(COFFSymbolRef Sym) {
  StringRef Name;
  COFFObj->getSymbolName(Sym, Name);
  return new (Alloc) Undefined(Name);
}

Undefined *ObjectFile::createWeakExternal(COFFSymbolRef Sym, const void *AuxP) {
  StringRef Name;
  COFFObj->getSymbolName(Sym, Name);
  auto *U = new (Alloc) Undefined(Name);
  auto *Aux = (const coff_aux_weak_external *)AuxP;
  U->WeakAlias = SparseSymbolBodies[Aux->TagIndex];
  return U;
}

Defined *ObjectFile::createDefined(COFFSymbolRef Sym, const void *AuxP,
                                   bool IsFirst) {
  StringRef Name;
  if (Sym.isCommon()) {
    auto *C = new (Alloc) CommonChunk(Sym);
    Chunks.push_back(C);
    return new (Alloc) DefinedCommon(this, Sym, C);
  }
  if (Sym.isAbsolute()) {
    COFFObj->getSymbolName(Sym, Name);
    // Skip special symbols.
    if (Name == "@comp.id" || Name == "@feat.00")
      return nullptr;
    return new (Alloc) DefinedAbsolute(Name, Sym);
  }
  if (Sym.getSectionNumber() == llvm::COFF::IMAGE_SYM_DEBUG)
    return nullptr;

  // Nothing else to do without a section chunk.
  auto *SC = cast_or_null<SectionChunk>(SparseChunks[Sym.getSectionNumber()]);
  if (!SC)
    return nullptr;

  // Handle associative sections
  if (IsFirst && AuxP) {
    auto *Aux = reinterpret_cast<const coff_aux_section_definition *>(AuxP);
    if (Aux->Selection == IMAGE_COMDAT_SELECT_ASSOCIATIVE)
      if (auto *ParentSC = cast_or_null<SectionChunk>(
              SparseChunks[Aux->getNumber(Sym.isBigObj())]))
        ParentSC->addAssociative(SC);
  }

  auto *B = new (Alloc) DefinedRegular(this, Sym, SC);
  if (SC->isCOMDAT() && Sym.getValue() == 0 && !AuxP)
    SC->setSymbol(B);

  return B;
}

std::error_code ImportFile::parse() {
  const char *Buf = MB.getBufferStart();
  const char *End = MB.getBufferEnd();
  const auto *Hdr = reinterpret_cast<const coff_import_header *>(Buf);

  // Check if the total size is valid.
  if ((size_t)(End - Buf) != (sizeof(*Hdr) + Hdr->SizeOfData)) {
    llvm::errs() << "broken import library\n";
    return make_error_code(LLDError::BrokenFile);
  }

  // Read names and create an __imp_ symbol.
  StringRef Name = StringAlloc.save(StringRef(Buf + sizeof(*Hdr)));
  StringRef ImpName = StringAlloc.save(Twine("__imp_") + Name);
  StringRef DLLName(Buf + sizeof(coff_import_header) + Name.size() + 1);
  StringRef ExternalName = Name;
  if (Hdr->getNameType() == llvm::COFF::IMPORT_ORDINAL)
    ExternalName = "";
  auto *ImpSym = new (Alloc) DefinedImportData(DLLName, ImpName, ExternalName,
                                               Hdr);
  SymbolBodies.push_back(ImpSym);

  // If type is function, we need to create a thunk which jump to an
  // address pointed by the __imp_ symbol. (This allows you to call
  // DLL functions just like regular non-DLL functions.)
  if (Hdr->getType() == llvm::COFF::IMPORT_CODE)
    SymbolBodies.push_back(new (Alloc) DefinedImportThunk(Name, ImpSym));
  return std::error_code();
}

std::error_code BitcodeFile::parse() {
  std::string Err;
  M.reset(LTOModule::createFromBuffer(MB.getBufferStart(),
                                      MB.getBufferSize(),
                                      llvm::TargetOptions(), Err));
  if (!Err.empty()) {
    llvm::errs() << Err << '\n';
    return make_error_code(LLDError::BrokenFile);
  }

  llvm::BumpPtrStringSaver Saver(Alloc);
  for (unsigned I = 0, E = M->getSymbolCount(); I != E; ++I) {
    lto_symbol_attributes Attrs = M->getSymbolAttributes(I);
    if ((Attrs & LTO_SYMBOL_SCOPE_MASK) == LTO_SYMBOL_SCOPE_INTERNAL)
      continue;

    StringRef SymName = Saver.save(M->getSymbolName(I));
    int SymbolDef = Attrs & LTO_SYMBOL_DEFINITION_MASK;
    if (SymbolDef == LTO_SYMBOL_DEFINITION_UNDEFINED) {
      SymbolBodies.push_back(new (Alloc) Undefined(SymName));
    } else {
      bool Replaceable =
          (SymbolDef == LTO_SYMBOL_DEFINITION_TENTATIVE || // common
           (Attrs & LTO_SYMBOL_COMDAT) ||                  // comdat
           (SymbolDef == LTO_SYMBOL_DEFINITION_WEAK &&     // weak external
            (Attrs & LTO_SYMBOL_ALIAS)));
      SymbolBodies.push_back(new (Alloc) DefinedBitcode(this, SymName,
                                                        Replaceable));
    }
  }

  Directives = M->getLinkerOpts();
  return std::error_code();
}

} // namespace coff
} // namespace lld
