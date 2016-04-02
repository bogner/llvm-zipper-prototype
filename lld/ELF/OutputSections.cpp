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
#include "LinkerScript.h"
#include "SymbolTable.h"
#include "Target.h"
#include "lld/Core/Parallel.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/MathExtras.h"
#include <map>

using namespace llvm;
using namespace llvm::dwarf;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;

using namespace lld;
using namespace lld::elf;

static bool isAlpha(char C) {
  return ('a' <= C && C <= 'z') || ('A' <= C && C <= 'Z') || C == '_';
}

static bool isAlnum(char C) { return isAlpha(C) || ('0' <= C && C <= '9'); }

// Returns true if S is valid as a C language identifier.
bool elf::isValidCIdentifier(StringRef S) {
  return !S.empty() && isAlpha(S[0]) &&
         std::all_of(S.begin() + 1, S.end(), isAlnum);
}

template <class ELFT>
OutputSectionBase<ELFT>::OutputSectionBase(StringRef Name, uint32_t Type,
                                           uintX_t Flags)
    : Name(Name) {
  memset(&Header, 0, sizeof(Elf_Shdr));
  Header.sh_type = Type;
  Header.sh_flags = Flags;
}

template <class ELFT>
void OutputSectionBase<ELFT>::writeHeaderTo(Elf_Shdr *Shdr) {
  *Shdr = Header;
}

template <class ELFT>
GotPltSection<ELFT>::GotPltSection()
    : OutputSectionBase<ELFT>(".got.plt", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE) {
  this->Header.sh_addralign = sizeof(uintX_t);
}

template <class ELFT> void GotPltSection<ELFT>::addEntry(SymbolBody &Sym) {
  Sym.GotPltIndex = Target->GotPltHeaderEntriesNum + Entries.size();
  Entries.push_back(&Sym);
}

template <class ELFT> bool GotPltSection<ELFT>::empty() const {
  return Entries.empty();
}

template <class ELFT> void GotPltSection<ELFT>::finalize() {
  this->Header.sh_size =
      (Target->GotPltHeaderEntriesNum + Entries.size()) * sizeof(uintX_t);
}

template <class ELFT> void GotPltSection<ELFT>::writeTo(uint8_t *Buf) {
  Target->writeGotPltHeader(Buf);
  Buf += Target->GotPltHeaderEntriesNum * sizeof(uintX_t);
  for (const SymbolBody *B : Entries) {
    Target->writeGotPlt(Buf, B->getPltVA<ELFT>());
    Buf += sizeof(uintX_t);
  }
}

template <class ELFT>
GotSection<ELFT>::GotSection()
    : OutputSectionBase<ELFT>(".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE) {
  if (Config->EMachine == EM_MIPS)
    this->Header.sh_flags |= SHF_MIPS_GPREL;
  this->Header.sh_addralign = sizeof(uintX_t);
}

template <class ELFT> void GotSection<ELFT>::addEntry(SymbolBody &Sym) {
  if (Config->EMachine == EM_MIPS) {
    // For "true" local symbols which can be referenced from the same module
    // only compiler creates two instructions for address loading:
    //
    // lw   $8, 0($gp) # R_MIPS_GOT16
    // addi $8, $8, 0  # R_MIPS_LO16
    //
    // The first instruction loads high 16 bits of the symbol address while
    // the second adds an offset. That allows to reduce number of required
    // GOT entries because only one global offset table entry is necessary
    // for every 64 KBytes of local data. So for local symbols we need to
    // allocate number of GOT entries to hold all required "page" addresses.
    //
    // All global symbols (hidden and regular) considered by compiler uniformly.
    // It always generates a single `lw` instruction and R_MIPS_GOT16 relocation
    // to load address of the symbol. So for each such symbol we need to
    // allocate dedicated GOT entry to store its address.
    //
    // If a symbol is preemptible we need help of dynamic linker to get its
    // final address. The corresponding GOT entries are allocated in the
    // "global" part of GOT. Entries for non preemptible global symbol allocated
    // in the "local" part of GOT.
    //
    // See "Global Offset Table" in Chapter 5:
    // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
    if (Sym.isLocal()) {
      // At this point we do not know final symbol value so to reduce number
      // of allocated GOT entries do the following trick. Save all output
      // sections referenced by GOT relocations. Then later in the `finalize`
      // method calculate number of "pages" required to cover all saved output
      // section and allocate appropriate number of GOT entries.
      auto *OutSec = cast<DefinedRegular<ELFT>>(&Sym)->Section->OutSec;
      MipsOutSections.insert(OutSec);
      return;
    }
    if (!Sym.isPreemptible()) {
      // In case of non-local symbols require an entry in the local part
      // of MIPS GOT, we set GotIndex to 1 just to accent that this symbol
      // has the GOT entry and escape creation more redundant GOT entries.
      // FIXME (simon): We can try to store such symbols in the `Entries`
      // container. But in that case we have to sort out that container
      // and update GotIndex assigned to symbols.
      Sym.GotIndex = 1;
      ++MipsLocalEntries;
      return;
    }
    // All preemptible symbols with MIPS GOT entries should be represented
    // in the dynamic symbols table.
    Sym.MustBeInDynSym = true;
  }
  Sym.GotIndex = Entries.size();
  Entries.push_back(&Sym);
}

template <class ELFT> bool GotSection<ELFT>::addDynTlsEntry(SymbolBody &Sym) {
  if (Sym.hasGlobalDynIndex())
    return false;
  Sym.GlobalDynIndex = Target->GotHeaderEntriesNum + Entries.size();
  // Global Dynamic TLS entries take two GOT slots.
  Entries.push_back(&Sym);
  Entries.push_back(nullptr);
  return true;
}

// Reserves TLS entries for a TLS module ID and a TLS block offset.
// In total it takes two GOT slots.
template <class ELFT> bool GotSection<ELFT>::addTlsIndex() {
  if (TlsIndexOff != uint32_t(-1))
    return false;
  TlsIndexOff = Entries.size() * sizeof(uintX_t);
  Entries.push_back(nullptr);
  Entries.push_back(nullptr);
  return true;
}

template <class ELFT>
typename GotSection<ELFT>::uintX_t
GotSection<ELFT>::getMipsLocalFullAddr(const SymbolBody &B) {
  return getMipsLocalEntryAddr(B.getVA<ELFT>());
}

template <class ELFT>
typename GotSection<ELFT>::uintX_t
GotSection<ELFT>::getMipsLocalPageAddr(uintX_t EntryValue) {
  // Initialize the entry by the %hi(EntryValue) expression
  // but without right-shifting.
  return getMipsLocalEntryAddr((EntryValue + 0x8000) & ~0xffff);
}

template <class ELFT>
typename GotSection<ELFT>::uintX_t
GotSection<ELFT>::getMipsLocalEntryAddr(uintX_t EntryValue) {
  size_t NewIndex = Target->GotHeaderEntriesNum + MipsLocalGotPos.size();
  auto P = MipsLocalGotPos.insert(std::make_pair(EntryValue, NewIndex));
  assert(!P.second || MipsLocalGotPos.size() <= MipsLocalEntries);
  return this->getVA() + P.first->second * sizeof(uintX_t);
}

template <class ELFT>
typename GotSection<ELFT>::uintX_t
GotSection<ELFT>::getGlobalDynAddr(const SymbolBody &B) const {
  return this->getVA() + B.GlobalDynIndex * sizeof(uintX_t);
}

template <class ELFT>
const SymbolBody *GotSection<ELFT>::getMipsFirstGlobalEntry() const {
  return Entries.empty() ? nullptr : Entries.front();
}

template <class ELFT>
unsigned GotSection<ELFT>::getMipsLocalEntriesNum() const {
  return Target->GotHeaderEntriesNum + MipsLocalEntries;
}

template <class ELFT> void GotSection<ELFT>::finalize() {
  for (const OutputSectionBase<ELFT> *OutSec : MipsOutSections) {
    // Calculate an upper bound of MIPS GOT entries required to store page
    // addresses of local symbols. We assume the worst case - each 64kb
    // page of the output section has at least one GOT relocation against it.
    // Add 0x8000 to the section's size because the page address stored
    // in the GOT entry is calculated as (value + 0x8000) & ~0xffff.
    MipsLocalEntries += (OutSec->getSize() + 0x8000 + 0xfffe) / 0xffff;
  }
  this->Header.sh_size =
      (Target->GotHeaderEntriesNum + MipsLocalEntries + Entries.size()) *
      sizeof(uintX_t);
}

template <class ELFT> void GotSection<ELFT>::writeTo(uint8_t *Buf) {
  Target->writeGotHeader(Buf);
  for (std::pair<uintX_t, size_t> &L : MipsLocalGotPos) {
    uint8_t *Entry = Buf + L.second * sizeof(uintX_t);
    write<uintX_t, ELFT::TargetEndianness, sizeof(uintX_t)>(Entry, L.first);
  }
  Buf += Target->GotHeaderEntriesNum * sizeof(uintX_t);
  Buf += MipsLocalEntries * sizeof(uintX_t);
  for (const SymbolBody *B : Entries) {
    uint8_t *Entry = Buf;
    Buf += sizeof(uintX_t);
    if (!B)
      continue;
    // MIPS has special rules to fill up GOT entries.
    // See "Global Offset Table" in Chapter 5 in the following document
    // for detailed description:
    // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
    // As the first approach, we can just store addresses for all symbols.
    if (Config->EMachine != EM_MIPS && B->isPreemptible())
      continue; // The dynamic linker will take care of it.
    uintX_t VA = B->getVA<ELFT>();
    write<uintX_t, ELFT::TargetEndianness, sizeof(uintX_t)>(Entry, VA);
  }
}

template <class ELFT>
PltSection<ELFT>::PltSection()
    : OutputSectionBase<ELFT>(".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR) {
  this->Header.sh_addralign = 16;
}

template <class ELFT> void PltSection<ELFT>::writeTo(uint8_t *Buf) {
  size_t Off = 0;
  if (Target->UseLazyBinding) {
    // At beginning of PLT, we have code to call the dynamic linker
    // to resolve dynsyms at runtime. Write such code.
    Target->writePltZero(Buf);
    Off += Target->PltZeroSize;
  }
  for (auto &I : Entries) {
    const SymbolBody *B = I.first;
    unsigned RelOff = I.second;
    uint64_t Got =
        Target->UseLazyBinding ? B->getGotPltVA<ELFT>() : B->getGotVA<ELFT>();
    uint64_t Plt = this->getVA() + Off;
    Target->writePlt(Buf + Off, Got, Plt, B->PltIndex, RelOff);
    Off += Target->PltEntrySize;
  }
}

template <class ELFT> void PltSection<ELFT>::addEntry(SymbolBody &Sym) {
  Sym.PltIndex = Entries.size();
  unsigned RelOff = Target->UseLazyBinding
                        ? Out<ELFT>::RelaPlt->getRelocOffset()
                        : Out<ELFT>::RelaDyn->getRelocOffset();
  Entries.push_back(std::make_pair(&Sym, RelOff));
}

template <class ELFT> void PltSection<ELFT>::finalize() {
  this->Header.sh_size =
      Target->PltZeroSize + Entries.size() * Target->PltEntrySize;
}

template <class ELFT>
RelocationSection<ELFT>::RelocationSection(StringRef Name)
    : OutputSectionBase<ELFT>(Name, Config->Rela ? SHT_RELA : SHT_REL,
                              SHF_ALLOC) {
  this->Header.sh_entsize = Config->Rela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
  this->Header.sh_addralign = sizeof(uintX_t);
}

template <class ELFT>
void RelocationSection<ELFT>::addReloc(const DynamicReloc<ELFT> &Reloc) {
  SymbolBody *Sym = Reloc.Sym;
  if (!Reloc.UseSymVA && Sym)
    Sym->MustBeInDynSym = true;
  Relocs.push_back(Reloc);
}

template <class ELFT>
typename ELFT::uint DynamicReloc<ELFT>::getOffset() const {
  switch (OKind) {
  case Off_GTlsIndex:
    return Out<ELFT>::Got->getGlobalDynAddr(*Sym);
  case Off_GTlsOffset:
    return Out<ELFT>::Got->getGlobalDynAddr(*Sym) + sizeof(uintX_t);
  case Off_LTlsIndex:
    return Out<ELFT>::Got->getTlsIndexVA();
  case Off_Sec:
    return OffsetSec->getOffset(OffsetInSec) + OffsetSec->OutSec->getVA();
  case Off_Bss:
    return cast<SharedSymbol<ELFT>>(Sym)->OffsetInBss + Out<ELFT>::Bss->getVA();
  case Off_Got:
    return Sym->getGotVA<ELFT>();
  case Off_GotPlt:
    return Sym->getGotPltVA<ELFT>();
  }
  llvm_unreachable("invalid offset kind");
}

template <class ELFT> void RelocationSection<ELFT>::writeTo(uint8_t *Buf) {
  for (const DynamicReloc<ELFT> &Rel : Relocs) {
    auto *P = reinterpret_cast<Elf_Rela *>(Buf);
    Buf += Config->Rela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
    SymbolBody *Sym = Rel.Sym;

    if (Config->Rela)
      P->r_addend = Rel.UseSymVA ? Sym->getVA<ELFT>(Rel.Addend) : Rel.Addend;
    P->r_offset = Rel.getOffset();
    uint32_t SymIdx = (!Rel.UseSymVA && Sym) ? Sym->DynsymIndex : 0;
    P->setSymbolAndType(SymIdx, Rel.Type, Config->Mips64EL);
  }
}

template <class ELFT> unsigned RelocationSection<ELFT>::getRelocOffset() {
  return this->Header.sh_entsize * Relocs.size();
}

template <class ELFT> void RelocationSection<ELFT>::finalize() {
  this->Header.sh_link = Static ? Out<ELFT>::SymTab->SectionIndex
                                : Out<ELFT>::DynSymTab->SectionIndex;
  this->Header.sh_size = Relocs.size() * this->Header.sh_entsize;
}

template <class ELFT>
InterpSection<ELFT>::InterpSection()
    : OutputSectionBase<ELFT>(".interp", SHT_PROGBITS, SHF_ALLOC) {
  this->Header.sh_size = Config->DynamicLinker.size() + 1;
  this->Header.sh_addralign = 1;
}

template <class ELFT> void InterpSection<ELFT>::writeTo(uint8_t *Buf) {
  StringRef S = Config->DynamicLinker;
  memcpy(Buf, S.data(), S.size());
}

template <class ELFT>
HashTableSection<ELFT>::HashTableSection()
    : OutputSectionBase<ELFT>(".hash", SHT_HASH, SHF_ALLOC) {
  this->Header.sh_entsize = sizeof(Elf_Word);
  this->Header.sh_addralign = sizeof(Elf_Word);
}

static uint32_t hashSysv(StringRef Name) {
  uint32_t H = 0;
  for (char C : Name) {
    H = (H << 4) + C;
    uint32_t G = H & 0xf0000000;
    if (G)
      H ^= G >> 24;
    H &= ~G;
  }
  return H;
}

template <class ELFT> void HashTableSection<ELFT>::finalize() {
  this->Header.sh_link = Out<ELFT>::DynSymTab->SectionIndex;

  unsigned NumEntries = 2;                             // nbucket and nchain.
  NumEntries += Out<ELFT>::DynSymTab->getNumSymbols(); // The chain entries.

  // Create as many buckets as there are symbols.
  // FIXME: This is simplistic. We can try to optimize it, but implementing
  // support for SHT_GNU_HASH is probably even more profitable.
  NumEntries += Out<ELFT>::DynSymTab->getNumSymbols();
  this->Header.sh_size = NumEntries * sizeof(Elf_Word);
}

template <class ELFT> void HashTableSection<ELFT>::writeTo(uint8_t *Buf) {
  unsigned NumSymbols = Out<ELFT>::DynSymTab->getNumSymbols();
  auto *P = reinterpret_cast<Elf_Word *>(Buf);
  *P++ = NumSymbols; // nbucket
  *P++ = NumSymbols; // nchain

  Elf_Word *Buckets = P;
  Elf_Word *Chains = P + NumSymbols;

  for (const std::pair<SymbolBody *, unsigned> &P :
       Out<ELFT>::DynSymTab->getSymbols()) {
    SymbolBody *Body = P.first;
    StringRef Name = Body->getName();
    unsigned I = Body->DynsymIndex;
    uint32_t Hash = hashSysv(Name) % NumSymbols;
    Chains[I] = Buckets[Hash];
    Buckets[Hash] = I;
  }
}

static uint32_t hashGnu(StringRef Name) {
  uint32_t H = 5381;
  for (uint8_t C : Name)
    H = (H << 5) + H + C;
  return H;
}

template <class ELFT>
GnuHashTableSection<ELFT>::GnuHashTableSection()
    : OutputSectionBase<ELFT>(".gnu.hash", SHT_GNU_HASH, SHF_ALLOC) {
  this->Header.sh_entsize = ELFT::Is64Bits ? 0 : 4;
  this->Header.sh_addralign = sizeof(uintX_t);
}

template <class ELFT>
unsigned GnuHashTableSection<ELFT>::calcNBuckets(unsigned NumHashed) {
  if (!NumHashed)
    return 0;

  // These values are prime numbers which are not greater than 2^(N-1) + 1.
  // In result, for any particular NumHashed we return a prime number
  // which is not greater than NumHashed.
  static const unsigned Primes[] = {
      1,   1,    3,    3,    7,    13,    31,    61,    127,   251,
      509, 1021, 2039, 4093, 8191, 16381, 32749, 65521, 131071};

  return Primes[std::min<unsigned>(Log2_32_Ceil(NumHashed),
                                   array_lengthof(Primes) - 1)];
}

// Bloom filter estimation: at least 8 bits for each hashed symbol.
// GNU Hash table requirement: it should be a power of 2,
//   the minimum value is 1, even for an empty table.
// Expected results for a 32-bit target:
//   calcMaskWords(0..4)   = 1
//   calcMaskWords(5..8)   = 2
//   calcMaskWords(9..16)  = 4
// For a 64-bit target:
//   calcMaskWords(0..8)   = 1
//   calcMaskWords(9..16)  = 2
//   calcMaskWords(17..32) = 4
template <class ELFT>
unsigned GnuHashTableSection<ELFT>::calcMaskWords(unsigned NumHashed) {
  if (!NumHashed)
    return 1;
  return NextPowerOf2((NumHashed - 1) / sizeof(Elf_Off));
}

template <class ELFT> void GnuHashTableSection<ELFT>::finalize() {
  unsigned NumHashed = Symbols.size();
  NBuckets = calcNBuckets(NumHashed);
  MaskWords = calcMaskWords(NumHashed);
  // Second hash shift estimation: just predefined values.
  Shift2 = ELFT::Is64Bits ? 6 : 5;

  this->Header.sh_link = Out<ELFT>::DynSymTab->SectionIndex;
  this->Header.sh_size = sizeof(Elf_Word) * 4            // Header
                         + sizeof(Elf_Off) * MaskWords   // Bloom Filter
                         + sizeof(Elf_Word) * NBuckets   // Hash Buckets
                         + sizeof(Elf_Word) * NumHashed; // Hash Values
}

template <class ELFT> void GnuHashTableSection<ELFT>::writeTo(uint8_t *Buf) {
  writeHeader(Buf);
  if (Symbols.empty())
    return;
  writeBloomFilter(Buf);
  writeHashTable(Buf);
}

template <class ELFT>
void GnuHashTableSection<ELFT>::writeHeader(uint8_t *&Buf) {
  auto *P = reinterpret_cast<Elf_Word *>(Buf);
  *P++ = NBuckets;
  *P++ = Out<ELFT>::DynSymTab->getNumSymbols() - Symbols.size();
  *P++ = MaskWords;
  *P++ = Shift2;
  Buf = reinterpret_cast<uint8_t *>(P);
}

template <class ELFT>
void GnuHashTableSection<ELFT>::writeBloomFilter(uint8_t *&Buf) {
  unsigned C = sizeof(Elf_Off) * 8;

  auto *Masks = reinterpret_cast<Elf_Off *>(Buf);
  for (const SymbolData &Sym : Symbols) {
    size_t Pos = (Sym.Hash / C) & (MaskWords - 1);
    uintX_t V = (uintX_t(1) << (Sym.Hash % C)) |
                (uintX_t(1) << ((Sym.Hash >> Shift2) % C));
    Masks[Pos] |= V;
  }
  Buf += sizeof(Elf_Off) * MaskWords;
}

template <class ELFT>
void GnuHashTableSection<ELFT>::writeHashTable(uint8_t *Buf) {
  Elf_Word *Buckets = reinterpret_cast<Elf_Word *>(Buf);
  Elf_Word *Values = Buckets + NBuckets;

  int PrevBucket = -1;
  int I = 0;
  for (const SymbolData &Sym : Symbols) {
    int Bucket = Sym.Hash % NBuckets;
    assert(PrevBucket <= Bucket);
    if (Bucket != PrevBucket) {
      Buckets[Bucket] = Sym.Body->DynsymIndex;
      PrevBucket = Bucket;
      if (I > 0)
        Values[I - 1] |= 1;
    }
    Values[I] = Sym.Hash & ~1;
    ++I;
  }
  if (I > 0)
    Values[I - 1] |= 1;
}

static bool includeInGnuHashTable(SymbolBody *B) {
  // Assume that includeInDynsym() is already checked.
  return !B->isUndefined();
}

// Add symbols to this symbol hash table. Note that this function
// destructively sort a given vector -- which is needed because
// GNU-style hash table places some sorting requirements.
template <class ELFT>
void GnuHashTableSection<ELFT>::addSymbols(
    std::vector<std::pair<SymbolBody *, size_t>> &V) {
  auto Mid = std::stable_partition(V.begin(), V.end(),
                                   [](std::pair<SymbolBody *, size_t> &P) {
                                     return !includeInGnuHashTable(P.first);
                                   });
  if (Mid == V.end())
    return;
  for (auto I = Mid, E = V.end(); I != E; ++I) {
    SymbolBody *B = I->first;
    size_t StrOff = I->second;
    Symbols.push_back({B, StrOff, hashGnu(B->getName())});
  }

  unsigned NBuckets = calcNBuckets(Symbols.size());
  std::stable_sort(Symbols.begin(), Symbols.end(),
                   [&](const SymbolData &L, const SymbolData &R) {
                     return L.Hash % NBuckets < R.Hash % NBuckets;
                   });

  V.erase(Mid, V.end());
  for (const SymbolData &Sym : Symbols)
    V.push_back({Sym.Body, Sym.STName});
}

template <class ELFT>
DynamicSection<ELFT>::DynamicSection(SymbolTable<ELFT> &SymTab)
    : OutputSectionBase<ELFT>(".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE),
      SymTab(SymTab) {
  Elf_Shdr &Header = this->Header;
  Header.sh_addralign = sizeof(uintX_t);
  Header.sh_entsize = ELFT::Is64Bits ? 16 : 8;

  // .dynamic section is not writable on MIPS.
  // See "Special Section" in Chapter 4 in the following document:
  // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
  if (Config->EMachine == EM_MIPS)
    Header.sh_flags = SHF_ALLOC;
}

template <class ELFT> void DynamicSection<ELFT>::finalize() {
  if (this->Header.sh_size)
    return; // Already finalized.

  Elf_Shdr &Header = this->Header;
  Header.sh_link = Out<ELFT>::DynStrTab->SectionIndex;

  auto Add = [=](Entry E) { Entries.push_back(E); };

  // Add strings. We know that these are the last strings to be added to
  // DynStrTab and doing this here allows this function to set DT_STRSZ.
  if (!Config->RPath.empty())
    Add({Config->EnableNewDtags ? DT_RUNPATH : DT_RPATH,
         Out<ELFT>::DynStrTab->addString(Config->RPath)});
  for (const std::unique_ptr<SharedFile<ELFT>> &F : SymTab.getSharedFiles())
    if (F->isNeeded())
      Add({DT_NEEDED, Out<ELFT>::DynStrTab->addString(F->getSoName())});
  if (!Config->SoName.empty())
    Add({DT_SONAME, Out<ELFT>::DynStrTab->addString(Config->SoName)});

  Out<ELFT>::DynStrTab->finalize();

  if (Out<ELFT>::RelaDyn->hasRelocs()) {
    bool IsRela = Config->Rela;
    Add({IsRela ? DT_RELA : DT_REL, Out<ELFT>::RelaDyn});
    Add({IsRela ? DT_RELASZ : DT_RELSZ, Out<ELFT>::RelaDyn->getSize()});
    Add({IsRela ? DT_RELAENT : DT_RELENT,
         uintX_t(IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel))});
  }
  if (Out<ELFT>::RelaPlt && Out<ELFT>::RelaPlt->hasRelocs()) {
    Add({DT_JMPREL, Out<ELFT>::RelaPlt});
    Add({DT_PLTRELSZ, Out<ELFT>::RelaPlt->getSize()});
    Add({Config->EMachine == EM_MIPS ? DT_MIPS_PLTGOT : DT_PLTGOT,
         Out<ELFT>::GotPlt});
    Add({DT_PLTREL, uint64_t(Config->Rela ? DT_RELA : DT_REL)});
  }

  Add({DT_SYMTAB, Out<ELFT>::DynSymTab});
  Add({DT_SYMENT, sizeof(Elf_Sym)});
  Add({DT_STRTAB, Out<ELFT>::DynStrTab});
  Add({DT_STRSZ, Out<ELFT>::DynStrTab->getSize()});
  if (Out<ELFT>::GnuHashTab)
    Add({DT_GNU_HASH, Out<ELFT>::GnuHashTab});
  if (Out<ELFT>::HashTab)
    Add({DT_HASH, Out<ELFT>::HashTab});

  if (PreInitArraySec) {
    Add({DT_PREINIT_ARRAY, PreInitArraySec});
    Add({DT_PREINIT_ARRAYSZ, PreInitArraySec->getSize()});
  }
  if (InitArraySec) {
    Add({DT_INIT_ARRAY, InitArraySec});
    Add({DT_INIT_ARRAYSZ, (uintX_t)InitArraySec->getSize()});
  }
  if (FiniArraySec) {
    Add({DT_FINI_ARRAY, FiniArraySec});
    Add({DT_FINI_ARRAYSZ, (uintX_t)FiniArraySec->getSize()});
  }

  if (SymbolBody *B = SymTab.find(Config->Init))
    Add({DT_INIT, B});
  if (SymbolBody *B = SymTab.find(Config->Fini))
    Add({DT_FINI, B});

  uint32_t DtFlags = 0;
  uint32_t DtFlags1 = 0;
  if (Config->Bsymbolic)
    DtFlags |= DF_SYMBOLIC;
  if (Config->ZNodelete)
    DtFlags1 |= DF_1_NODELETE;
  if (Config->ZNow) {
    DtFlags |= DF_BIND_NOW;
    DtFlags1 |= DF_1_NOW;
  }
  if (Config->ZOrigin) {
    DtFlags |= DF_ORIGIN;
    DtFlags1 |= DF_1_ORIGIN;
  }

  if (DtFlags)
    Add({DT_FLAGS, DtFlags});
  if (DtFlags1)
    Add({DT_FLAGS_1, DtFlags1});

  if (!Config->Entry.empty())
    Add({DT_DEBUG, (uint64_t)0});

  if (Config->EMachine == EM_MIPS) {
    Add({DT_MIPS_RLD_VERSION, 1});
    Add({DT_MIPS_FLAGS, RHF_NOTPOT});
    Add({DT_MIPS_BASE_ADDRESS, (uintX_t)Target->getVAStart()});
    Add({DT_MIPS_SYMTABNO, Out<ELFT>::DynSymTab->getNumSymbols()});
    Add({DT_MIPS_LOCAL_GOTNO, Out<ELFT>::Got->getMipsLocalEntriesNum()});
    if (const SymbolBody *B = Out<ELFT>::Got->getMipsFirstGlobalEntry())
      Add({DT_MIPS_GOTSYM, B->DynsymIndex});
    else
      Add({DT_MIPS_GOTSYM, Out<ELFT>::DynSymTab->getNumSymbols()});
    Add({DT_PLTGOT, Out<ELFT>::Got});
    if (Out<ELFT>::MipsRldMap)
      Add({DT_MIPS_RLD_MAP, Out<ELFT>::MipsRldMap});
  }

  // +1 for DT_NULL
  Header.sh_size = (Entries.size() + 1) * Header.sh_entsize;
}

template <class ELFT> void DynamicSection<ELFT>::writeTo(uint8_t *Buf) {
  auto *P = reinterpret_cast<Elf_Dyn *>(Buf);

  for (const Entry &E : Entries) {
    P->d_tag = E.Tag;
    switch (E.Kind) {
    case Entry::SecAddr:
      P->d_un.d_ptr = E.OutSec->getVA();
      break;
    case Entry::SymAddr:
      P->d_un.d_ptr = E.Sym->template getVA<ELFT>();
      break;
    case Entry::PlainInt:
      P->d_un.d_val = E.Val;
      break;
    }
    ++P;
  }
}

template <class ELFT>
EhFrameHeader<ELFT>::EhFrameHeader()
    : OutputSectionBase<ELFT>(".eh_frame_hdr", llvm::ELF::SHT_PROGBITS,
                              SHF_ALLOC) {
  // It's a 4 bytes of header + pointer to the contents of the .eh_frame section
  // + the number of FDE pointers in the table.
  this->Header.sh_size = 12;
}

// We have to get PC values of FDEs. They depend on relocations
// which are target specific, so we run this code after performing
// all relocations. We read the values from ouput buffer according to the
// encoding given for FDEs. Return value is an offset to the initial PC value
// for the FDE.
template <class ELFT>
typename EhFrameHeader<ELFT>::uintX_t
EhFrameHeader<ELFT>::getFdePc(uintX_t EhVA, const FdeData &F) {
  const endianness E = ELFT::TargetEndianness;
  uint8_t Size = F.Enc & 0x7;
  if (Size == DW_EH_PE_absptr)
    Size = sizeof(uintX_t) == 8 ? DW_EH_PE_udata8 : DW_EH_PE_udata4;
  uint64_t PC;
  switch (Size) {
  case DW_EH_PE_udata2:
    PC = read16<E>(F.PCRel);
    break;
  case DW_EH_PE_udata4:
    PC = read32<E>(F.PCRel);
    break;
  case DW_EH_PE_udata8:
    PC = read64<E>(F.PCRel);
    break;
  default:
    fatal("unknown FDE size encoding");
  }
  switch (F.Enc & 0x70) {
  case DW_EH_PE_absptr:
    return PC;
  case DW_EH_PE_pcrel:
    return PC + EhVA + F.Off + 8;
  default:
    fatal("unknown FDE size relative encoding");
  }
}

template <class ELFT> void EhFrameHeader<ELFT>::writeTo(uint8_t *Buf) {
  const endianness E = ELFT::TargetEndianness;

  const uint8_t Header[] = {1, DW_EH_PE_pcrel | DW_EH_PE_sdata4,
                            DW_EH_PE_udata4,
                            DW_EH_PE_datarel | DW_EH_PE_sdata4};
  memcpy(Buf, Header, sizeof(Header));

  uintX_t EhVA = Sec->getVA();
  uintX_t VA = this->getVA();
  uintX_t EhOff = EhVA - VA - 4;
  write32<E>(Buf + 4, EhOff);
  write32<E>(Buf + 8, this->FdeList.size());
  Buf += 12;

  // InitialPC -> Offset in .eh_frame, sorted by InitialPC.
  std::map<uintX_t, size_t> PcToOffset;
  for (const FdeData &F : FdeList)
    PcToOffset[getFdePc(EhVA, F)] = F.Off;

  for (auto &I : PcToOffset) {
    // The first four bytes are an offset to the initial PC value for the FDE.
    write32<E>(Buf, I.first - VA);
    // The last four bytes are an offset to the FDE data itself.
    write32<E>(Buf + 4, EhVA + I.second - VA);
    Buf += 8;
  }
}

template <class ELFT>
void EhFrameHeader<ELFT>::assignEhFrame(EHOutputSection<ELFT> *Sec) {
  assert((!this->Sec || this->Sec == Sec) &&
         "multiple .eh_frame sections not supported for .eh_frame_hdr");
  Live = Config->EhFrameHdr;
  this->Sec = Sec;
}

template <class ELFT>
void EhFrameHeader<ELFT>::addFde(uint8_t Enc, size_t Off, uint8_t *PCRel) {
  if (Live && (Enc & 0xF0) == DW_EH_PE_datarel)
    fatal("DW_EH_PE_datarel encoding unsupported for FDEs by .eh_frame_hdr");
  FdeList.push_back(FdeData{Enc, Off, PCRel});
}

template <class ELFT> void EhFrameHeader<ELFT>::reserveFde() {
  // Each FDE entry is 8 bytes long:
  // The first four bytes are an offset to the initial PC value for the FDE. The
  // last four byte are an offset to the FDE data itself.
  this->Header.sh_size += 8;
}

template <class ELFT>
OutputSection<ELFT>::OutputSection(StringRef Name, uint32_t Type, uintX_t Flags)
    : OutputSectionBase<ELFT>(Name, Type, Flags) {
  if (Type == SHT_RELA)
    this->Header.sh_entsize = sizeof(Elf_Rela);
  else if (Type == SHT_REL)
    this->Header.sh_entsize = sizeof(Elf_Rel);
}

template <class ELFT> void OutputSection<ELFT>::finalize() {
  uint32_t Type = this->Header.sh_type;
  if (Type != SHT_RELA && Type != SHT_REL)
    return;
  this->Header.sh_link = Out<ELFT>::SymTab->SectionIndex;
  // sh_info for SHT_REL[A] sections should contain the section header index of
  // the section to which the relocation applies.
  InputSectionBase<ELFT> *S = Sections[0]->getRelocatedSection();
  this->Header.sh_info = S->OutSec->SectionIndex;
}

template <class ELFT>
void OutputSection<ELFT>::addSection(InputSectionBase<ELFT> *C) {
  assert(C->Live);
  auto *S = cast<InputSection<ELFT>>(C);
  Sections.push_back(S);
  S->OutSec = this;
  this->updateAlign(S->Align);
}

// If an input string is in the form of "foo.N" where N is a number,
// return N. Otherwise, returns 65536, which is one greater than the
// lowest priority.
static int getPriority(StringRef S) {
  size_t Pos = S.rfind('.');
  if (Pos == StringRef::npos)
    return 65536;
  int V;
  if (S.substr(Pos + 1).getAsInteger(10, V))
    return 65536;
  return V;
}

// This function is called after we sort input sections
// and scan relocations to setup sections' offsets.
template <class ELFT> void OutputSection<ELFT>::assignOffsets() {
  uintX_t Off = 0;
  for (InputSection<ELFT> *S : Sections) {
    Off = alignTo(Off, S->Align);
    S->OutSecOff = Off;
    Off += S->getSize();
  }
  this->Header.sh_size = Off;
}

// Sorts input sections by section name suffixes, so that .foo.N comes
// before .foo.M if N < M. Used to sort .{init,fini}_array.N sections.
// We want to keep the original order if the priorities are the same
// because the compiler keeps the original initialization order in a
// translation unit and we need to respect that.
// For more detail, read the section of the GCC's manual about init_priority.
template <class ELFT> void OutputSection<ELFT>::sortInitFini() {
  // Sort sections by priority.
  typedef std::pair<int, InputSection<ELFT> *> Pair;
  auto Comp = [](const Pair &A, const Pair &B) { return A.first < B.first; };

  std::vector<Pair> V;
  for (InputSection<ELFT> *S : Sections)
    V.push_back({getPriority(S->getSectionName()), S});
  std::stable_sort(V.begin(), V.end(), Comp);
  Sections.clear();
  for (Pair &P : V)
    Sections.push_back(P.second);
}

// Returns true if S matches /Filename.?\.o$/.
static bool isCrtBeginEnd(StringRef S, StringRef Filename) {
  if (!S.endswith(".o"))
    return false;
  S = S.drop_back(2);
  if (S.endswith(Filename))
    return true;
  return !S.empty() && S.drop_back().endswith(Filename);
}

static bool isCrtbegin(StringRef S) { return isCrtBeginEnd(S, "crtbegin"); }
static bool isCrtend(StringRef S) { return isCrtBeginEnd(S, "crtend"); }

// .ctors and .dtors are sorted by this priority from highest to lowest.
//
//  1. The section was contained in crtbegin (crtbegin contains
//     some sentinel value in its .ctors and .dtors so that the runtime
//     can find the beginning of the sections.)
//
//  2. The section has an optional priority value in the form of ".ctors.N"
//     or ".dtors.N" where N is a number. Unlike .{init,fini}_array,
//     they are compared as string rather than number.
//
//  3. The section is just ".ctors" or ".dtors".
//
//  4. The section was contained in crtend, which contains an end marker.
//
// In an ideal world, we don't need this function because .init_array and
// .ctors are duplicate features (and .init_array is newer.) However, there
// are too many real-world use cases of .ctors, so we had no choice to
// support that with this rather ad-hoc semantics.
template <class ELFT>
static bool compCtors(const InputSection<ELFT> *A,
                      const InputSection<ELFT> *B) {
  bool BeginA = isCrtbegin(A->getFile()->getName());
  bool BeginB = isCrtbegin(B->getFile()->getName());
  if (BeginA != BeginB)
    return BeginA;
  bool EndA = isCrtend(A->getFile()->getName());
  bool EndB = isCrtend(B->getFile()->getName());
  if (EndA != EndB)
    return EndB;
  StringRef X = A->getSectionName();
  StringRef Y = B->getSectionName();
  assert(X.startswith(".ctors") || X.startswith(".dtors"));
  assert(Y.startswith(".ctors") || Y.startswith(".dtors"));
  X = X.substr(6);
  Y = Y.substr(6);
  if (X.empty() && Y.empty())
    return false;
  return X < Y;
}

// Sorts input sections by the special rules for .ctors and .dtors.
// Unfortunately, the rules are different from the one for .{init,fini}_array.
// Read the comment above.
template <class ELFT> void OutputSection<ELFT>::sortCtorsDtors() {
  std::stable_sort(Sections.begin(), Sections.end(), compCtors<ELFT>);
}

static void fill(uint8_t *Buf, size_t Size, ArrayRef<uint8_t> A) {
  size_t I = 0;
  for (; I + A.size() < Size; I += A.size())
    memcpy(Buf + I, A.data(), A.size());
  memcpy(Buf + I, A.data(), Size - I);
}

template <class ELFT> void OutputSection<ELFT>::writeTo(uint8_t *Buf) {
  ArrayRef<uint8_t> Filler = Script->getFiller(this->Name);
  if (!Filler.empty())
    fill(Buf, this->getSize(), Filler);
  if (Config->Threads) {
    parallel_for_each(Sections.begin(), Sections.end(),
                      [=](InputSection<ELFT> *C) { C->writeTo(Buf); });
  } else {
    for (InputSection<ELFT> *C : Sections)
      C->writeTo(Buf);
  }
}

template <class ELFT>
EHOutputSection<ELFT>::EHOutputSection(StringRef Name, uint32_t Type,
                                       uintX_t Flags)
    : OutputSectionBase<ELFT>(Name, Type, Flags) {
  Out<ELFT>::EhFrameHdr->assignEhFrame(this);
}

template <class ELFT>
EHRegion<ELFT>::EHRegion(EHInputSection<ELFT> *S, unsigned Index)
    : S(S), Index(Index) {}

template <class ELFT> StringRef EHRegion<ELFT>::data() const {
  ArrayRef<uint8_t> SecData = S->getSectionData();
  ArrayRef<std::pair<uintX_t, uintX_t>> Offsets = S->Offsets;
  size_t Start = Offsets[Index].first;
  size_t End =
      Index == Offsets.size() - 1 ? SecData.size() : Offsets[Index + 1].first;
  return StringRef((const char *)SecData.data() + Start, End - Start);
}

template <class ELFT>
Cie<ELFT>::Cie(EHInputSection<ELFT> *S, unsigned Index)
    : EHRegion<ELFT>(S, Index) {}

// Read a byte and advance D by one byte.
static uint8_t readByte(ArrayRef<uint8_t> &D) {
  if (D.empty())
    fatal("corrupted or unsupported CIE information");
  uint8_t B = D.front();
  D = D.slice(1);
  return B;
}

static void skipLeb128(ArrayRef<uint8_t> &D) {
  while (!D.empty()) {
    uint8_t Val = D.front();
    D = D.slice(1);
    if ((Val & 0x80) == 0)
      return;
  }
  fatal("corrupted or unsupported CIE information");
}

template <class ELFT> static size_t getAugPSize(unsigned Enc) {
  switch (Enc & 0x0f) {
  case DW_EH_PE_absptr:
  case DW_EH_PE_signed:
    return ELFT::Is64Bits ? 8 : 4;
  case DW_EH_PE_udata2:
  case DW_EH_PE_sdata2:
    return 2;
  case DW_EH_PE_udata4:
  case DW_EH_PE_sdata4:
    return 4;
  case DW_EH_PE_udata8:
  case DW_EH_PE_sdata8:
    return 8;
  }
  fatal("unknown FDE encoding");
}

template <class ELFT> static void skipAugP(ArrayRef<uint8_t> &D) {
  uint8_t Enc = readByte(D);
  if ((Enc & 0xf0) == DW_EH_PE_aligned)
    fatal("DW_EH_PE_aligned encoding is not supported");
  size_t Size = getAugPSize<ELFT>(Enc);
  if (Size >= D.size())
    fatal("corrupted CIE");
  D = D.slice(Size);
}

template <class ELFT>
uint8_t EHOutputSection<ELFT>::getFdeEncoding(ArrayRef<uint8_t> D) {
  if (D.size() < 8)
    fatal("CIE too small");
  D = D.slice(8);

  uint8_t Version = readByte(D);
  if (Version != 1 && Version != 3)
    fatal("FDE version 1 or 3 expected, but got " + Twine((unsigned)Version));

  const unsigned char *AugEnd = std::find(D.begin() + 1, D.end(), '\0');
  if (AugEnd == D.end())
    fatal("corrupted CIE");
  StringRef Aug(reinterpret_cast<const char *>(D.begin()), AugEnd - D.begin());
  D = D.slice(Aug.size() + 1);

  // Code alignment factor should always be 1 for .eh_frame.
  if (readByte(D) != 1)
    fatal("CIE code alignment must be 1");

  // Skip data alignment factor.
  skipLeb128(D);

  // Skip the return address register. In CIE version 1 this is a single
  // byte. In CIE version 3 this is an unsigned LEB128.
  if (Version == 1)
    readByte(D);
  else
    skipLeb128(D);

  // We only care about an 'R' value, but other records may precede an 'R'
  // record. Records are not in TLV (type-length-value) format, so we need
  // to teach the linker how to skip records for each type.
  for (char C : Aug) {
    if (C == 'R')
      return readByte(D);
    if (C == 'z') {
      skipLeb128(D);
      continue;
    }
    if (C == 'P') {
      skipAugP<ELFT>(D);
      continue;
    }
    if (C == 'L') {
      readByte(D);
      continue;
    }
    fatal("unknown .eh_frame augmentation string: " + Aug);
  }
  return DW_EH_PE_absptr;
}

template <class ELFT>
static typename ELFT::uint readEntryLength(ArrayRef<uint8_t> D) {
  const endianness E = ELFT::TargetEndianness;
  if (D.size() < 4)
    fatal("CIE/FDE too small");

  // First 4 bytes of CIE/FDE is the size of the record.
  // If it is 0xFFFFFFFF, the next 8 bytes contain the size instead.
  uint64_t V = read32<E>(D.data());
  if (V < UINT32_MAX) {
    uint64_t Len = V + 4;
    if (Len > D.size())
      fatal("CIE/FIE ends past the end of the section");
    return Len;
  }

  if (D.size() < 12)
    fatal("CIE/FDE too small");
  V = read64<E>(D.data() + 4);
  uint64_t Len = V + 12;
  if (Len < V || D.size() < Len)
    fatal("CIE/FIE ends past the end of the section");
  return Len;
}

template <class ELFT>
template <class RelTy>
void EHOutputSection<ELFT>::addSectionAux(EHInputSection<ELFT> *S,
                                          iterator_range<const RelTy *> Rels) {
  const endianness E = ELFT::TargetEndianness;

  S->OutSec = this;
  this->updateAlign(S->Align);
  Sections.push_back(S);

  ArrayRef<uint8_t> SecData = S->getSectionData();
  ArrayRef<uint8_t> D = SecData;
  uintX_t Offset = 0;
  auto RelI = Rels.begin();
  auto RelE = Rels.end();

  DenseMap<unsigned, unsigned> OffsetToIndex;
  while (!D.empty()) {
    unsigned Index = S->Offsets.size();
    S->Offsets.push_back(std::make_pair(Offset, -1));

    uintX_t Length = readEntryLength<ELFT>(D);
    // If CIE/FDE data length is zero then Length is 4, this
    // shall be considered a terminator and processing shall end.
    if (Length == 4)
      break;
    StringRef Entry((const char *)D.data(), Length);

    while (RelI != RelE && RelI->r_offset < Offset)
      ++RelI;
    uintX_t NextOffset = Offset + Length;
    bool HasReloc = RelI != RelE && RelI->r_offset < NextOffset;

    uint32_t ID = read32<E>(D.data() + 4);
    if (ID == 0) {
      // CIE
      Cie<ELFT> C(S, Index);
      if (Config->EhFrameHdr)
        C.FdeEncoding = getFdeEncoding(D);

      SymbolBody *Personality = nullptr;
      if (HasReloc) {
        uint32_t SymIndex = RelI->getSymbol(Config->Mips64EL);
        Personality = &S->getFile()->getSymbolBody(SymIndex).repl();
      }

      std::pair<StringRef, SymbolBody *> CieInfo(Entry, Personality);
      auto P = CieMap.insert(std::make_pair(CieInfo, Cies.size()));
      if (P.second) {
        Cies.push_back(C);
        this->Header.sh_size += alignTo(Length, sizeof(uintX_t));
      }
      OffsetToIndex[Offset] = P.first->second;
    } else {
      if (!HasReloc)
        fatal("FDE doesn't reference another section");
      InputSectionBase<ELFT> *Target = S->getRelocTarget(*RelI);
      if (Target && Target->Live) {
        uint32_t CieOffset = Offset + 4 - ID;
        auto I = OffsetToIndex.find(CieOffset);
        if (I == OffsetToIndex.end())
          fatal("invalid CIE reference");
        Cies[I->second].Fdes.push_back(EHRegion<ELFT>(S, Index));
        Out<ELFT>::EhFrameHdr->reserveFde();
        this->Header.sh_size += alignTo(Length, sizeof(uintX_t));
      }
    }

    Offset = NextOffset;
    D = D.slice(Length);
  }
}

template <class ELFT>
void EHOutputSection<ELFT>::addSection(InputSectionBase<ELFT> *C) {
  auto *S = cast<EHInputSection<ELFT>>(C);
  const Elf_Shdr *RelSec = S->RelocSection;
  if (!RelSec) {
    addSectionAux(S, make_range<const Elf_Rela *>(nullptr, nullptr));
    return;
  }
  ELFFile<ELFT> &Obj = S->getFile()->getObj();
  if (RelSec->sh_type == SHT_RELA)
    addSectionAux(S, Obj.relas(RelSec));
  else
    addSectionAux(S, Obj.rels(RelSec));
}

template <class ELFT>
static typename ELFT::uint writeAlignedCieOrFde(StringRef Data, uint8_t *Buf) {
  typedef typename ELFT::uint uintX_t;
  const endianness E = ELFT::TargetEndianness;
  uint64_t Len = alignTo(Data.size(), sizeof(uintX_t));
  write32<E>(Buf, Len - 4);
  memcpy(Buf + 4, Data.data() + 4, Data.size() - 4);
  return Len;
}

template <class ELFT> void EHOutputSection<ELFT>::writeTo(uint8_t *Buf) {
  const endianness E = ELFT::TargetEndianness;
  size_t Offset = 0;
  for (const Cie<ELFT> &C : Cies) {
    size_t CieOffset = Offset;

    uintX_t CIELen = writeAlignedCieOrFde<ELFT>(C.data(), Buf + Offset);
    C.S->Offsets[C.Index].second = Offset;
    Offset += CIELen;

    for (const EHRegion<ELFT> &F : C.Fdes) {
      uintX_t Len = writeAlignedCieOrFde<ELFT>(F.data(), Buf + Offset);
      write32<E>(Buf + Offset + 4, Offset + 4 - CieOffset); // Pointer
      F.S->Offsets[F.Index].second = Offset;
      Out<ELFT>::EhFrameHdr->addFde(C.FdeEncoding, Offset, Buf + Offset + 8);
      Offset += Len;
    }
  }

  for (EHInputSection<ELFT> *S : Sections) {
    const Elf_Shdr *RelSec = S->RelocSection;
    if (!RelSec)
      continue;
    ELFFile<ELFT> &EObj = S->getFile()->getObj();
    if (RelSec->sh_type == SHT_RELA)
      S->relocate(Buf, nullptr, EObj.relas(RelSec));
    else
      S->relocate(Buf, nullptr, EObj.rels(RelSec));
  }
}

template <class ELFT>
MergeOutputSection<ELFT>::MergeOutputSection(StringRef Name, uint32_t Type,
                                             uintX_t Flags, uintX_t Alignment)
    : OutputSectionBase<ELFT>(Name, Type, Flags),
      Builder(llvm::StringTableBuilder::RAW, Alignment) {}

template <class ELFT> void MergeOutputSection<ELFT>::writeTo(uint8_t *Buf) {
  if (shouldTailMerge()) {
    StringRef Data = Builder.data();
    memcpy(Buf, Data.data(), Data.size());
    return;
  }
  for (const std::pair<StringRef, size_t> &P : Builder.getMap()) {
    StringRef Data = P.first;
    memcpy(Buf + P.second, Data.data(), Data.size());
  }
}

static size_t findNull(StringRef S, size_t EntSize) {
  // Optimize the common case.
  if (EntSize == 1)
    return S.find(0);

  for (unsigned I = 0, N = S.size(); I != N; I += EntSize) {
    const char *B = S.begin() + I;
    if (std::all_of(B, B + EntSize, [](char C) { return C == 0; }))
      return I;
  }
  return StringRef::npos;
}

template <class ELFT>
void MergeOutputSection<ELFT>::addSection(InputSectionBase<ELFT> *C) {
  auto *S = cast<MergeInputSection<ELFT>>(C);
  S->OutSec = this;
  this->updateAlign(S->Align);

  ArrayRef<uint8_t> D = S->getSectionData();
  StringRef Data((const char *)D.data(), D.size());
  uintX_t EntSize = S->getSectionHdr()->sh_entsize;
  this->Header.sh_entsize = EntSize;

  // If this is of type string, the contents are null-terminated strings.
  if (this->Header.sh_flags & SHF_STRINGS) {
    uintX_t Offset = 0;
    while (!Data.empty()) {
      size_t End = findNull(Data, EntSize);
      if (End == StringRef::npos)
        fatal("string is not null terminated");
      StringRef Entry = Data.substr(0, End + EntSize);
      uintX_t OutputOffset = Builder.add(Entry);
      if (shouldTailMerge())
        OutputOffset = -1;
      S->Offsets.push_back(std::make_pair(Offset, OutputOffset));
      uintX_t Size = End + EntSize;
      Data = Data.substr(Size);
      Offset += Size;
    }
    return;
  }

  // If this is not of type string, every entry has the same size.
  for (unsigned I = 0, N = Data.size(); I != N; I += EntSize) {
    StringRef Entry = Data.substr(I, EntSize);
    size_t OutputOffset = Builder.add(Entry);
    S->Offsets.push_back(std::make_pair(I, OutputOffset));
  }
}

template <class ELFT>
unsigned MergeOutputSection<ELFT>::getOffset(StringRef Val) {
  return Builder.getOffset(Val);
}

template <class ELFT> bool MergeOutputSection<ELFT>::shouldTailMerge() const {
  return Config->Optimize >= 2 && this->Header.sh_flags & SHF_STRINGS;
}

template <class ELFT> void MergeOutputSection<ELFT>::finalize() {
  if (shouldTailMerge())
    Builder.finalize();
  this->Header.sh_size = Builder.getSize();
}

template <class ELFT>
StringTableSection<ELFT>::StringTableSection(StringRef Name, bool Dynamic)
    : OutputSectionBase<ELFT>(Name, SHT_STRTAB,
                              Dynamic ? (uintX_t)SHF_ALLOC : 0),
      Dynamic(Dynamic) {
  this->Header.sh_addralign = 1;
}

// Adds a string to the string table. If HashIt is true we hash and check for
// duplicates. It is optional because the name of global symbols are already
// uniqued and hashing them again has a big cost for a small value: uniquing
// them with some other string that happens to be the same.
template <class ELFT>
unsigned StringTableSection<ELFT>::addString(StringRef S, bool HashIt) {
  if (HashIt) {
    auto R = StringMap.insert(std::make_pair(S, Size));
    if (!R.second)
      return R.first->second;
  }
  unsigned Ret = Size;
  Size += S.size() + 1;
  Strings.push_back(S);
  return Ret;
}

template <class ELFT> void StringTableSection<ELFT>::writeTo(uint8_t *Buf) {
  // ELF string tables start with NUL byte, so advance the pointer by one.
  ++Buf;
  for (StringRef S : Strings) {
    memcpy(Buf, S.data(), S.size());
    Buf += S.size() + 1;
  }
}

template <class ELFT>
SymbolTableSection<ELFT>::SymbolTableSection(
    SymbolTable<ELFT> &Table, StringTableSection<ELFT> &StrTabSec)
    : OutputSectionBase<ELFT>(StrTabSec.isDynamic() ? ".dynsym" : ".symtab",
                              StrTabSec.isDynamic() ? SHT_DYNSYM : SHT_SYMTAB,
                              StrTabSec.isDynamic() ? (uintX_t)SHF_ALLOC : 0),
      StrTabSec(StrTabSec), Table(Table) {
  this->Header.sh_entsize = sizeof(Elf_Sym);
  this->Header.sh_addralign = sizeof(uintX_t);
}

// Orders symbols according to their positions in the GOT,
// in compliance with MIPS ABI rules.
// See "Global Offset Table" in Chapter 5 in the following document
// for detailed description:
// ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
static bool sortMipsSymbols(const std::pair<SymbolBody *, unsigned> &L,
                            const std::pair<SymbolBody *, unsigned> &R) {
  // Sort entries related to non-local preemptible symbols by GOT indexes.
  // All other entries go to the first part of GOT in arbitrary order.
  bool LIsInLocalGot = !L.first->isInGot() || !L.first->isPreemptible();
  bool RIsInLocalGot = !R.first->isInGot() || !R.first->isPreemptible();
  if (LIsInLocalGot || RIsInLocalGot)
    return !RIsInLocalGot;
  return L.first->GotIndex < R.first->GotIndex;
}

template <class ELFT> void SymbolTableSection<ELFT>::finalize() {
  if (this->Header.sh_size)
    return; // Already finalized.

  this->Header.sh_size = getNumSymbols() * sizeof(Elf_Sym);
  this->Header.sh_link = StrTabSec.SectionIndex;
  this->Header.sh_info = NumLocals + 1;

  if (Config->Relocatable) {
    size_t I = NumLocals;
    for (const std::pair<SymbolBody *, size_t> &P : Symbols)
      P.first->DynsymIndex = ++I;
    return;
  }

  if (!StrTabSec.isDynamic()) {
    std::stable_sort(Symbols.begin(), Symbols.end(),
                     [](const std::pair<SymbolBody *, unsigned> &L,
                        const std::pair<SymbolBody *, unsigned> &R) {
                       return getSymbolBinding(L.first) == STB_LOCAL &&
                              getSymbolBinding(R.first) != STB_LOCAL;
                     });
    return;
  }
  if (Out<ELFT>::GnuHashTab)
    // NB: It also sorts Symbols to meet the GNU hash table requirements.
    Out<ELFT>::GnuHashTab->addSymbols(Symbols);
  else if (Config->EMachine == EM_MIPS)
    std::stable_sort(Symbols.begin(), Symbols.end(), sortMipsSymbols);
  size_t I = 0;
  for (const std::pair<SymbolBody *, size_t> &P : Symbols)
    P.first->DynsymIndex = ++I;
}

template <class ELFT>
void SymbolTableSection<ELFT>::addSymbol(SymbolBody *B) {
  Symbols.push_back({B, StrTabSec.addString(B->getName(), false)});
}

template <class ELFT> void SymbolTableSection<ELFT>::writeTo(uint8_t *Buf) {
  Buf += sizeof(Elf_Sym);

  // All symbols with STB_LOCAL binding precede the weak and global symbols.
  // .dynsym only contains global symbols.
  if (!Config->DiscardAll && !StrTabSec.isDynamic())
    writeLocalSymbols(Buf);

  writeGlobalSymbols(Buf);
}

template <class ELFT>
void SymbolTableSection<ELFT>::writeLocalSymbols(uint8_t *&Buf) {
  // Iterate over all input object files to copy their local symbols
  // to the output symbol table pointed by Buf.
  for (const std::unique_ptr<ObjectFile<ELFT>> &File : Table.getObjectFiles()) {
    for (const std::pair<const Elf_Sym *, size_t> &P : File->KeptLocalSyms) {
      const Elf_Sym *Sym = P.first;

      auto *ESym = reinterpret_cast<Elf_Sym *>(Buf);
      uintX_t VA = 0;
      if (Sym->st_shndx == SHN_ABS) {
        ESym->st_shndx = SHN_ABS;
        VA = Sym->st_value;
      } else {
        InputSectionBase<ELFT> *Section = File->getSection(*Sym);
        const OutputSectionBase<ELFT> *OutSec = Section->OutSec;
        ESym->st_shndx = OutSec->SectionIndex;
        VA = Section->getOffset(*Sym);
        VA += OutSec->getVA();
      }
      ESym->st_name = P.second;
      ESym->st_size = Sym->st_size;
      ESym->setBindingAndType(Sym->getBinding(), Sym->getType());
      ESym->st_value = VA;
      Buf += sizeof(*ESym);
    }
  }
}

template <class ELFT>
void SymbolTableSection<ELFT>::writeGlobalSymbols(uint8_t *Buf) {
  // Write the internal symbol table contents to the output symbol table
  // pointed by Buf.
  auto *ESym = reinterpret_cast<Elf_Sym *>(Buf);
  for (const std::pair<SymbolBody *, size_t> &P : Symbols) {
    SymbolBody *Body = P.first;
    size_t StrOff = P.second;

    uint8_t Type = STT_NOTYPE;
    uintX_t Size = 0;
    if (const Elf_Sym *InputSym = Body->getElfSym<ELFT>()) {
      Type = InputSym->getType();
      Size = InputSym->st_size;
    } else if (auto *C = dyn_cast<DefinedCommon>(Body)) {
      Type = STT_OBJECT;
      Size = C->Size;
    }

    ESym->setBindingAndType(getSymbolBinding(Body), Type);
    ESym->st_size = Size;
    ESym->st_name = StrOff;
    ESym->setVisibility(Body->getVisibility());
    ESym->st_value = Body->getVA<ELFT>();

    if (const OutputSectionBase<ELFT> *OutSec = getOutputSection(Body))
      ESym->st_shndx = OutSec->SectionIndex;
    else if (isa<DefinedRegular<ELFT>>(Body))
      ESym->st_shndx = SHN_ABS;

    // On MIPS we need to mark symbol which has a PLT entry and requires pointer
    // equality by STO_MIPS_PLT flag. That is necessary to help dynamic linker
    // distinguish such symbols and MIPS lazy-binding stubs.
    // https://sourceware.org/ml/binutils/2008-07/txt00000.txt
    if (Config->EMachine == EM_MIPS && Body->isInPlt() &&
        Body->NeedsCopyOrPltAddr)
      ESym->st_other |= STO_MIPS_PLT;
    ++ESym;
  }
}

template <class ELFT>
const OutputSectionBase<ELFT> *
SymbolTableSection<ELFT>::getOutputSection(SymbolBody *Sym) {
  switch (Sym->kind()) {
  case SymbolBody::DefinedSyntheticKind:
    return &cast<DefinedSynthetic<ELFT>>(Sym)->Section;
  case SymbolBody::DefinedRegularKind: {
    auto &D = cast<DefinedRegular<ELFT>>(Sym->repl());
    if (D.Section)
      return D.Section->OutSec;
    break;
  }
  case SymbolBody::DefinedCommonKind:
    return Out<ELFT>::Bss;
  case SymbolBody::SharedKind:
    if (cast<SharedSymbol<ELFT>>(Sym)->needsCopy())
      return Out<ELFT>::Bss;
    break;
  case SymbolBody::UndefinedElfKind:
  case SymbolBody::UndefinedKind:
  case SymbolBody::LazyKind:
    break;
  case SymbolBody::DefinedBitcodeKind:
    llvm_unreachable("should have been replaced");
  }
  return nullptr;
}

template <class ELFT>
uint8_t SymbolTableSection<ELFT>::getSymbolBinding(SymbolBody *Body) {
  uint8_t Visibility = Body->getVisibility();
  if (Visibility != STV_DEFAULT && Visibility != STV_PROTECTED)
    return STB_LOCAL;
  if (const Elf_Sym *ESym = Body->getElfSym<ELFT>())
    return ESym->getBinding();
  if (isa<DefinedSynthetic<ELFT>>(Body))
    return STB_LOCAL;
  return Body->isWeak() ? STB_WEAK : STB_GLOBAL;
}

template <class ELFT>
BuildIdSection<ELFT>::BuildIdSection()
    : OutputSectionBase<ELFT>(".note.gnu.build-id", SHT_NOTE, SHF_ALLOC) {
  // 16 bytes for the note section header and 8 bytes for FNV1 hash.
  this->Header.sh_size = 24;
}

template <class ELFT> void BuildIdSection<ELFT>::writeTo(uint8_t *Buf) {
  const endianness E = ELFT::TargetEndianness;
  write32<E>(Buf, 4);                   // Name size
  write32<E>(Buf + 4, sizeof(Hash));    // Content size
  write32<E>(Buf + 8, NT_GNU_BUILD_ID); // Type
  memcpy(Buf + 12, "GNU", 4);           // Name string
  HashBuf = Buf + 16;
}

template <class ELFT> void BuildIdSection<ELFT>::update(ArrayRef<uint8_t> Buf) {
  // 64-bit FNV1 hash
  const uint64_t Prime = 0x100000001b3;
  for (uint8_t B : Buf) {
    Hash *= Prime;
    Hash ^= B;
  }
}

template <class ELFT> void BuildIdSection<ELFT>::writeBuildId() {
  const endianness E = ELFT::TargetEndianness;
  write64<E>(HashBuf, Hash);
}

template <class ELFT>
MipsReginfoOutputSection<ELFT>::MipsReginfoOutputSection()
    : OutputSectionBase<ELFT>(".reginfo", SHT_MIPS_REGINFO, SHF_ALLOC) {
  this->Header.sh_addralign = 4;
  this->Header.sh_entsize = sizeof(Elf_Mips_RegInfo);
  this->Header.sh_size = sizeof(Elf_Mips_RegInfo);
}

template <class ELFT>
void MipsReginfoOutputSection<ELFT>::writeTo(uint8_t *Buf) {
  auto *R = reinterpret_cast<Elf_Mips_RegInfo *>(Buf);
  R->ri_gp_value = getMipsGpAddr<ELFT>();
  R->ri_gprmask = GprMask;
}

template <class ELFT>
void MipsReginfoOutputSection<ELFT>::addSection(InputSectionBase<ELFT> *C) {
  // Copy input object file's .reginfo gprmask to output.
  auto *S = cast<MipsReginfoInputSection<ELFT>>(C);
  GprMask |= S->Reginfo->ri_gprmask;
}

namespace lld {
namespace elf {
template class OutputSectionBase<ELF32LE>;
template class OutputSectionBase<ELF32BE>;
template class OutputSectionBase<ELF64LE>;
template class OutputSectionBase<ELF64BE>;

template class EhFrameHeader<ELF32LE>;
template class EhFrameHeader<ELF32BE>;
template class EhFrameHeader<ELF64LE>;
template class EhFrameHeader<ELF64BE>;

template class GotPltSection<ELF32LE>;
template class GotPltSection<ELF32BE>;
template class GotPltSection<ELF64LE>;
template class GotPltSection<ELF64BE>;

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

template class InterpSection<ELF32LE>;
template class InterpSection<ELF32BE>;
template class InterpSection<ELF64LE>;
template class InterpSection<ELF64BE>;

template class GnuHashTableSection<ELF32LE>;
template class GnuHashTableSection<ELF32BE>;
template class GnuHashTableSection<ELF64LE>;
template class GnuHashTableSection<ELF64BE>;

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

template class EHOutputSection<ELF32LE>;
template class EHOutputSection<ELF32BE>;
template class EHOutputSection<ELF64LE>;
template class EHOutputSection<ELF64BE>;

template class MipsReginfoOutputSection<ELF32LE>;
template class MipsReginfoOutputSection<ELF32BE>;
template class MipsReginfoOutputSection<ELF64LE>;
template class MipsReginfoOutputSection<ELF64BE>;

template class MergeOutputSection<ELF32LE>;
template class MergeOutputSection<ELF32BE>;
template class MergeOutputSection<ELF64LE>;
template class MergeOutputSection<ELF64BE>;

template class StringTableSection<ELF32LE>;
template class StringTableSection<ELF32BE>;
template class StringTableSection<ELF64LE>;
template class StringTableSection<ELF64BE>;

template class SymbolTableSection<ELF32LE>;
template class SymbolTableSection<ELF32BE>;
template class SymbolTableSection<ELF64LE>;
template class SymbolTableSection<ELF64BE>;

template class BuildIdSection<ELF32LE>;
template class BuildIdSection<ELF32BE>;
template class BuildIdSection<ELF64LE>;
template class BuildIdSection<ELF64BE>;
}
}
