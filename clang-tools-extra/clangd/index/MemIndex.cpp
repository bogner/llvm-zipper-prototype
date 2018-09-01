//===--- MemIndex.cpp - Dynamic in-memory symbol index. ----------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===-------------------------------------------------------------------===//

#include "MemIndex.h"
#include "../FuzzyMatch.h"
#include "../Logger.h"
#include <queue>

namespace clang {
namespace clangd {

static std::shared_ptr<MemIndex::OccurrenceMap>
getOccurrencesFromSlab(SymbolOccurrenceSlab OccurrencesSlab) {
  struct Snapshot {
    SymbolOccurrenceSlab Slab;
    MemIndex::OccurrenceMap Occurrences;
  };

  auto Snap = std::make_shared<Snapshot>();
  Snap->Slab = std::move(OccurrencesSlab);
  for (const auto &IDAndOccurrences : Snap->Slab) {
    auto &Occurrences = Snap->Occurrences[IDAndOccurrences.first];
    for (const auto &Occurrence : IDAndOccurrences.second)
      Occurrences.push_back(&Occurrence);
  }
  return {std::move(Snap), &Snap->Occurrences};
}

void MemIndex::build(std::shared_ptr<std::vector<const Symbol *>> Syms,
                     std::shared_ptr<OccurrenceMap> AllOccurrences) {
  assert(Syms && "Syms must be set when build MemIndex");
  assert(AllOccurrences && "Occurrences must be set when build MemIndex");
  llvm::DenseMap<SymbolID, const Symbol *> TempIndex;
  for (const Symbol *Sym : *Syms)
    TempIndex[Sym->ID] = Sym;

  // Swap out the old symbols and index.
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    Index = std::move(TempIndex);
    Symbols = std::move(Syms); // Release old symbols.
    Occurrences = std::move(AllOccurrences);
  }

  vlog("Built MemIndex with estimated memory usage {0} bytes.",
       estimateMemoryUsage());
}

std::unique_ptr<SymbolIndex> MemIndex::build(SymbolSlab Symbols,
                                             SymbolOccurrenceSlab Occurrences) {
  auto Idx = llvm::make_unique<MemIndex>();
  Idx->build(getSymbolsFromSlab(std::move(Symbols)),
             getOccurrencesFromSlab(std::move(Occurrences)));
  return std::move(Idx);
}

bool MemIndex::fuzzyFind(
    const FuzzyFindRequest &Req,
    llvm::function_ref<void(const Symbol &)> Callback) const {
  assert(!StringRef(Req.Query).contains("::") &&
         "There must be no :: in query.");

  std::priority_queue<std::pair<float, const Symbol *>> Top;
  FuzzyMatcher Filter(Req.Query);
  bool More = false;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    for (const auto Pair : Index) {
      const Symbol *Sym = Pair.second;

      // Exact match against all possible scopes.
      if (!Req.Scopes.empty() && !llvm::is_contained(Req.Scopes, Sym->Scope))
        continue;
      if (Req.RestrictForCodeCompletion && !Sym->IsIndexedForCodeCompletion)
        continue;

      if (auto Score = Filter.match(Sym->Name)) {
        Top.emplace(-*Score * quality(*Sym), Sym);
        if (Top.size() > Req.MaxCandidateCount) {
          More = true;
          Top.pop();
        }
      }
    }
    for (; !Top.empty(); Top.pop())
      Callback(*Top.top().second);
  }
  return More;
}

void MemIndex::lookup(const LookupRequest &Req,
                      llvm::function_ref<void(const Symbol &)> Callback) const {
  std::lock_guard<std::mutex> Lock(Mutex);
  for (const auto &ID : Req.IDs) {
    auto I = Index.find(ID);
    if (I != Index.end())
      Callback(*I->second);
  }
}

void MemIndex::findOccurrences(
    const OccurrencesRequest &Req,
    llvm::function_ref<void(const SymbolOccurrence &)> Callback) const {
  std::lock_guard<std::mutex> Lock(Mutex);
  for (const auto &ReqID : Req.IDs) {
    auto FoundOccurrences = Occurrences->find(ReqID);
    if (FoundOccurrences == Occurrences->end())
      continue;
    for (const auto *O : FoundOccurrences->second) {
      if (static_cast<int>(Req.Filter & O->Kind))
        Callback(*O);
    }
  }
}

std::shared_ptr<std::vector<const Symbol *>>
getSymbolsFromSlab(SymbolSlab Slab) {
  struct Snapshot {
    SymbolSlab Slab;
    std::vector<const Symbol *> Pointers;
  };
  auto Snap = std::make_shared<Snapshot>();
  Snap->Slab = std::move(Slab);
  for (auto &Sym : Snap->Slab)
    Snap->Pointers.push_back(&Sym);
  return std::shared_ptr<std::vector<const Symbol *>>(std::move(Snap),
                                                      &Snap->Pointers);
}

size_t MemIndex::estimateMemoryUsage() const {
  std::lock_guard<std::mutex> Lock(Mutex);
  return Index.getMemorySize();
}

} // namespace clangd
} // namespace clang
