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
#include "lld/Core/Error.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::COFF;
using llvm::RoundUpToAlignment;

namespace lld {
namespace coff {

SectionChunk::SectionChunk(ObjectFile *F, const coff_section *H, uint32_t SI)
    : File(F), Header(H), SectionIndex(SI) {
  // Initialize SectionName.
  File->getCOFFObj()->getSectionName(Header, SectionName);
  // Bit [20:24] contains section alignment.
  unsigned Shift = ((Header->Characteristics & 0xF00000) >> 20) - 1;
  Align = uint32_t(1) << Shift;
}

void SectionChunk::writeTo(uint8_t *Buf) {
  assert(hasData());
  ArrayRef<uint8_t> Data;
  File->getCOFFObj()->getSectionContents(Header, Data);
  memcpy(Buf + FileOff, Data.data(), Data.size());
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

void SectionChunk::applyRelocations(uint8_t *Buf) {
  for (const auto &I : getSectionRef().relocations()) {
    const coff_relocation *Rel = File->getCOFFObj()->getCOFFRelocation(I);
    applyReloc(Buf, Rel);
  }
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
}

void ImportThunkChunk::applyRelocations(uint8_t *Buf) {
  uint32_t Operand = ImpSymbol->getRVA() - RVA - getSize();
  // The first two bytes are a JMP instruction. Fill its operand.
  write32le(Buf + FileOff + 2, Operand);
}

HintNameChunk::HintNameChunk(StringRef N)
    : Name(N), Size(RoundUpToAlignment(Name.size() + 4, 2)) {}

void HintNameChunk::writeTo(uint8_t *Buf) {
  // The first two bytes is Hint/Name field.
  memcpy(Buf + FileOff + 2, Name.data(), Name.size());
}

void LookupChunk::applyRelocations(uint8_t *Buf) {
  write32le(Buf + FileOff, HintName->getRVA());
}

void DirectoryChunk::applyRelocations(uint8_t *Buf) {
  auto *E = (coff_import_directory_table_entry *)(Buf + FileOff);
  E->ImportLookupTableRVA = LookupTab->getRVA();
  E->NameRVA = DLLName->getRVA();
  E->ImportAddressTableRVA = AddressTab->getRVA();
}

ImportTable::ImportTable(StringRef N,
                         std::vector<DefinedImportData *> &Symbols) {
  DLLName = new StringChunk(N);
  DirTab = new DirectoryChunk(DLLName);
  for (DefinedImportData *S : Symbols)
    HintNameTables.push_back(new HintNameChunk(S->getExportName()));

  for (HintNameChunk *H : HintNameTables) {
    LookupTables.push_back(new LookupChunk(H));
    AddressTables.push_back(new LookupChunk(H));
  }

  for (int I = 0, E = Symbols.size(); I < E; ++I)
    Symbols[I]->setLocation(AddressTables[I]);

  DirTab->LookupTab = LookupTables[0];
  DirTab->AddressTab = AddressTables[0];
}

} // namespace coff
} // namespace lld
