//===- Chunks.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Chunks.h"
#include "InputFiles.h"
#include "Writer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::COFF;
using llvm::RoundUpToAlignment;

namespace lld {
namespace coff {

const size_t LookupChunk::Size = sizeof(uint64_t);
const size_t DirectoryChunk::Size = sizeof(ImportDirectoryTableEntry);

SectionChunk::SectionChunk(ObjectFile *F, const coff_section *H, uint32_t SI)
    : File(F), Header(H), SectionIndex(SI) {
  // Initialize SectionName.
  File->getCOFFObj()->getSectionName(Header, SectionName);
  // Bit [20:24] contains section alignment.
  unsigned Shift = ((Header->Characteristics & 0xF00000) >> 20) - 1;
  Align = uint32_t(1) << Shift;
}

void SectionChunk::writeTo(uint8_t *Buf) {
  if (!hasData())
    return;
  // Copy section contents from source object file to output file.
  ArrayRef<uint8_t> Data;
  File->getCOFFObj()->getSectionContents(Header, Data);
  memcpy(Buf + FileOff, Data.data(), Data.size());

  // Apply relocations.
  for (const auto &I : getSectionRef().relocations()) {
    const coff_relocation *Rel = File->getCOFFObj()->getCOFFRelocation(I);
    applyReloc(Buf, Rel);
  }
}

// Returns true if this chunk should be considered as a GC root.
bool SectionChunk::isRoot() {
  // COMDAT sections are live only when they are referenced by something else.
  if (isCOMDAT())
    return false;

  // Associative sections are live if their parent COMDATs are live,
  // and vice versa, so they are not considered live by themselves.
  if (IsAssocChild)
    return false;

  // Only code is subject of dead-stripping.
  return !(Header->Characteristics & IMAGE_SCN_CNT_CODE);
}

void SectionChunk::markLive() {
  if (Live)
    return;
  Live = true;

  // Mark all symbols listed in the relocation table for this section.
  for (const auto &I : getSectionRef().relocations()) {
    const coff_relocation *Rel = File->getCOFFObj()->getCOFFRelocation(I);
    SymbolBody *B = File->getSymbolBody(Rel->SymbolTableIndex);
    if (auto *Def = dyn_cast<Defined>(B))
      Def->markLive();
  }

  // Mark associative sections if any.
  for (Chunk *C : AssocChildren)
    C->markLive();
}

void SectionChunk::addAssociative(SectionChunk *Child) {
  Child->IsAssocChild = true;
  AssocChildren.push_back(Child);
}

static void add16(uint8_t *P, int32_t V) { write16le(P, read16le(P) + V); }
static void add32(uint8_t *P, int32_t V) { write32le(P, read32le(P) + V); }
static void add64(uint8_t *P, int64_t V) { write64le(P, read64le(P) + V); }

// Implements x64 PE/COFF relocations.
void SectionChunk::applyReloc(uint8_t *Buf, const coff_relocation *Rel) {
  using namespace llvm::COFF;
  uint8_t *Off = Buf + FileOff + Rel->VirtualAddress;
  SymbolBody *Body = File->getSymbolBody(Rel->SymbolTableIndex);
  uint64_t S = cast<Defined>(Body)->getRVA();
  uint64_t P = RVA + Rel->VirtualAddress;
  switch (Rel->Type) {
  case IMAGE_REL_AMD64_ADDR32:   add32(Off, S + Config->ImageBase); break;
  case IMAGE_REL_AMD64_ADDR64:   add64(Off, S + Config->ImageBase); break;
  case IMAGE_REL_AMD64_ADDR32NB: add32(Off, S); break;
  case IMAGE_REL_AMD64_REL32:    add32(Off, S - P - 4); break;
  case IMAGE_REL_AMD64_REL32_1:  add32(Off, S - P - 5); break;
  case IMAGE_REL_AMD64_REL32_2:  add32(Off, S - P - 6); break;
  case IMAGE_REL_AMD64_REL32_3:  add32(Off, S - P - 7); break;
  case IMAGE_REL_AMD64_REL32_4:  add32(Off, S - P - 8); break;
  case IMAGE_REL_AMD64_REL32_5:  add32(Off, S - P - 9); break;
  case IMAGE_REL_AMD64_SECTION:  add16(Off, Out->getSectionIndex()); break;
  case IMAGE_REL_AMD64_SECREL:   add32(Off, S - Out->getRVA()); break;
  default:
    llvm::report_fatal_error("Unsupported relocation type");
  }
}

bool SectionChunk::hasData() const {
  return !(Header->Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA);
}

uint32_t SectionChunk::getPermissions() const {
  return Header->Characteristics & PermMask;
}

bool SectionChunk::isCOMDAT() const {
  return Header->Characteristics & IMAGE_SCN_LNK_COMDAT;
}

// Prints "Discarded <symbol>" for all external function symbols.
void SectionChunk::printDiscardedMessage() {
  uint32_t E = File->getCOFFObj()->getNumberOfSymbols();
  for (uint32_t I = 0; I < E; ++I) {
    auto SrefOrErr = File->getCOFFObj()->getSymbol(I);
    COFFSymbolRef Sym = SrefOrErr.get();
    if (uint32_t(Sym.getSectionNumber()) != SectionIndex)
      continue;
    if (!Sym.isFunctionDefinition())
      continue;
    StringRef SymbolName;
    File->getCOFFObj()->getSymbolName(Sym, SymbolName);
    llvm::dbgs() << "Discarded " << SymbolName << " from "
                 << File->getShortName() << "\n";
    I += Sym.getNumberOfAuxSymbols();
  }
}

SectionRef SectionChunk::getSectionRef() {
  DataRefImpl Ref;
  Ref.p = uintptr_t(Header);
  return SectionRef(Ref, File->getCOFFObj());
}

uint32_t CommonChunk::getPermissions() const {
  using namespace llvm::COFF;
  return IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ |
         IMAGE_SCN_MEM_WRITE;
}

void StringChunk::writeTo(uint8_t *Buf) {
  memcpy(Buf + FileOff, Str.data(), Str.size());
}

void ImportThunkChunk::writeTo(uint8_t *Buf) {
  memcpy(Buf + FileOff, ImportThunkData, sizeof(ImportThunkData));
  // The first two bytes is a JMP instruction. Fill its operand.
  uint32_t Operand = ImpSymbol->getRVA() - RVA - getSize();
  write32le(Buf + FileOff + 2, Operand);
}

size_t HintNameChunk::getSize() const {
  // Starts with 2 byte Hint field, followed by a null-terminated string,
  // ends with 0 or 1 byte padding.
  return RoundUpToAlignment(Name.size() + 3, 2);
}

void HintNameChunk::writeTo(uint8_t *Buf) {
  write16le(Buf + FileOff, Hint);
  memcpy(Buf + FileOff + 2, Name.data(), Name.size());
}

void LookupChunk::writeTo(uint8_t *Buf) {
  write32le(Buf + FileOff, HintName->getRVA());
}

void OrdinalOnlyChunk::writeTo(uint8_t *Buf) {
  // An import-by-ordinal slot has MSB 1 to indicate that
  // this is import-by-ordinal (and not import-by-name).
  write64le(Buf + FileOff, (uint64_t(1) << 63) | Ordinal);
}

void DirectoryChunk::writeTo(uint8_t *Buf) {
  auto *E = (coff_import_directory_table_entry *)(Buf + FileOff);
  E->ImportLookupTableRVA = LookupTab->getRVA();
  E->NameRVA = DLLName->getRVA();
  E->ImportAddressTableRVA = AddressTab->getRVA();
}

// Returns a list of .idata contents.
// See Microsoft PE/COFF spec 5.4 for details.
std::vector<Chunk *> IdataContents::getChunks() {
  create();
  std::vector<Chunk *> V;
  // The loader assumes a specific order of data.
  // Add each type in the correct order.
  for (std::unique_ptr<Chunk> &C : Dirs)
    V.push_back(C.get());
  for (std::unique_ptr<Chunk> &C : Lookups)
    V.push_back(C.get());
  for (std::unique_ptr<Chunk> &C : Addresses)
    V.push_back(C.get());
  for (std::unique_ptr<Chunk> &C : Hints)
    V.push_back(C.get());
  for (auto &P : DLLNames) {
    std::unique_ptr<Chunk> &C = P.second;
    V.push_back(C.get());
  }
  return V;
}

void IdataContents::create() {
  // Group DLL-imported symbols by DLL name because that's how
  // symbols are layed out in the import descriptor table.
  std::map<StringRef, std::vector<DefinedImportData *>> Map;
  for (DefinedImportData *Sym : Imports)
    Map[Sym->getDLLName()].push_back(Sym);

  // Create .idata contents for each DLL.
  for (auto &P : Map) {
    StringRef Name = P.first;
    std::vector<DefinedImportData *> &Syms = P.second;

    // Sort symbols by name for each group.
    std::sort(Syms.begin(), Syms.end(),
              [](DefinedImportData *A, DefinedImportData *B) {
                return A->getName() < B->getName();
              });

    // Create lookup and address tables. If they have external names,
    // we need to create HintName chunks to store the names.
    // If they don't (if they are import-by-ordinals), we store only
    // ordinal values to the table.
    size_t Base = Lookups.size();
    for (DefinedImportData *S : Syms) {
      uint16_t Ord = S->getOrdinal();
      if (S->getExternalName().empty()) {
        Lookups.push_back(make_unique<OrdinalOnlyChunk>(Ord));
        Addresses.push_back(make_unique<OrdinalOnlyChunk>(Ord));
        continue;
      }
      auto C = make_unique<HintNameChunk>(S->getExternalName(), Ord);
      Lookups.push_back(make_unique<LookupChunk>(C.get()));
      Addresses.push_back(make_unique<LookupChunk>(C.get()));
      Hints.push_back(std::move(C));
    }
    // Terminate with null values.
    Lookups.push_back(make_unique<NullChunk>(sizeof(uint64_t)));
    Addresses.push_back(make_unique<NullChunk>(sizeof(uint64_t)));

    for (int I = 0, E = Syms.size(); I < E; ++I)
      Syms[I]->setLocation(Addresses[Base + I].get());

    // Create the import table header.
    if (!DLLNames.count(Name))
      DLLNames[Name] = make_unique<StringChunk>(Name);
    auto Dir = make_unique<DirectoryChunk>(DLLNames[Name].get());
    Dir->LookupTab = Lookups[Base].get();
    Dir->AddressTab = Addresses[Base].get();
    Dirs.push_back(std::move(Dir));
  }
  // Add null terminator.
  Dirs.push_back(make_unique<NullChunk>(DirectoryChunk::Size));
}

} // namespace coff
} // namespace lld
