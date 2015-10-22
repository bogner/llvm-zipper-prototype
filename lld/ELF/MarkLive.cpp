//===- MarkLive.cpp -------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements --gc-sections, which is a feature to remove unused
// sections from output. Unused sections are sections that are not reachable
// from known GC-root symbols or sections. Naturally the feature is
// implemented as a mark-sweep garbage collector.
//
// Here's how it works. Each InputSectionBase has a "Live" bit. The bit is off
// by default. Starting with GC-root symbols or sections, markLive function
// defined in this file visits all reachable sections to set their Live
// bits. Writer will then ignore sections whose Live bits are off, so that
// such sections are removed from output.
//
//===----------------------------------------------------------------------===//

#include "InputSection.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Writer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/ELF.h"
#include <functional>
#include <vector>

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;

using namespace lld;
using namespace lld::elf2;

template <class ELFT, bool isRela>
static void
doForEachSuccessor(InputSectionBase<ELFT> *Sec,
                   std::function<void(InputSectionBase<ELFT> *)> Fn,
                   iterator_range<const Elf_Rel_Impl<ELFT, isRela> *> Rels) {
  typedef typename ELFFile<ELFT>::Elf_Sym Elf_Sym;
  typedef Elf_Rel_Impl<ELFT, isRela> RelType;

  ObjectFile<ELFT> *File = Sec->getFile();
  for (const RelType &RI : Rels) {
    // Global symbol
    uint32_t SymIndex = RI.getSymbol(Config->Mips64EL);
    if (SymbolBody *B = File->getSymbolBody(SymIndex)) {
      if (auto *D = dyn_cast<DefinedRegular<ELFT>>(B->repl()))
        Fn(&D->Section);
      continue;
    }
    // Local symbol
    if (const Elf_Sym *Sym = File->getLocalSymbol(SymIndex))
      if (InputSectionBase<ELFT> *Sec = File->getSection(*Sym))
        Fn(Sec);
  }
}

// Calls Fn for each section that Sec refers to.
template <class ELFT>
static void forEachSuccessor(InputSection<ELFT> *Sec,
                             std::function<void(InputSectionBase<ELFT> *)> Fn) {
  typedef typename ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  for (const Elf_Shdr *RelSec : Sec->RelocSections) {
    if (RelSec->sh_type == SHT_RELA)
      doForEachSuccessor(Sec, Fn, Sec->getFile()->getObj().relas(RelSec));
    else
      doForEachSuccessor(Sec, Fn, Sec->getFile()->getObj().rels(RelSec));
  }
}

// Sections listed below are special because they are used by the loader
// just by being in an ELF file. They should not be garbage-collected.
template <class ELFT> static bool isReserved(InputSectionBase<ELFT> *Sec) {
  switch (Sec->getSectionHdr()->sh_type) {
  case SHT_FINI_ARRAY:
  case SHT_INIT_ARRAY:
  case SHT_NOTE:
  case SHT_PREINIT_ARRAY:
    return true;
  default:
    StringRef S = Sec->getSectionName();
    return S.startswith(".init") || S.startswith(".fini") ||
           S.startswith(".jcr") || S == ".eh_frame";
  }
}

template <class ELFT> void lld::elf2::markLive(SymbolTable<ELFT> *Symtab) {
  SmallVector<InputSectionBase<ELFT> *, 256> Q;

  auto Enqueue = [&](InputSectionBase<ELFT> *Sec) {
    if (!Sec || Sec->Live)
      return;
    Sec->Live = true;
    Q.push_back(Sec);
  };

  auto MarkSymbol = [&](SymbolBody *Sym) {
    if (Sym)
      if (auto *D = dyn_cast<DefinedRegular<ELFT>>(Sym->repl()))
        Enqueue(&D->Section);
  };

  // Add GC root symbols.
  MarkSymbol(Config->EntrySym);
  MarkSymbol(Symtab->find(Config->Init));
  MarkSymbol(Symtab->find(Config->Fini));
  for (StringRef S : Config->Undefined)
    MarkSymbol(Symtab->find(S));

  // Preserve externally-visible symbols if the symbols defined by this
  // file could override other ELF file's symbols at runtime.
  if (Config->Shared || Config->ExportDynamic) {
    for (const std::pair<StringRef, Symbol *> &P : Symtab->getSymbols()) {
      SymbolBody *B = P.second->Body;
      if (B->getVisibility() == STV_DEFAULT)
        MarkSymbol(B);
    }
  }

  // Preserve special sections.
  for (const std::unique_ptr<ObjectFile<ELFT>> &F : Symtab->getObjectFiles())
    for (InputSectionBase<ELFT> *Sec : F->getSections())
      if (Sec && Sec != &InputSection<ELFT>::Discarded)
        if (isReserved(Sec))
          Enqueue(Sec);

  // Mark all reachable sections.
  while (!Q.empty())
    if (auto *Sec = dyn_cast<InputSection<ELFT>>(Q.pop_back_val()))
      forEachSuccessor<ELFT>(Sec, Enqueue);
}

template void lld::elf2::markLive<ELF32LE>(SymbolTable<ELF32LE> *);
template void lld::elf2::markLive<ELF32BE>(SymbolTable<ELF32BE> *);
template void lld::elf2::markLive<ELF64LE>(SymbolTable<ELF64LE> *);
template void lld::elf2::markLive<ELF64BE>(SymbolTable<ELF64BE> *);
