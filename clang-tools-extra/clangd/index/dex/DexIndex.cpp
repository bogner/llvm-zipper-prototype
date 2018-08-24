//===--- DexIndex.cpp - Dex Symbol Index Implementation ---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "DexIndex.h"
#include "../../FuzzyMatch.h"
#include "../../Logger.h"
#include <algorithm>
#include <queue>

namespace clang {
namespace clangd {
namespace dex {

namespace {

// Returns the tokens which are given symbol's characteristics. Currently, the
// generated tokens only contain fuzzy matching trigrams and symbol's scope,
// but in the future this will also return path proximity tokens and other
// types of tokens such as symbol type (if applicable).
// Returns the tokens which are given symbols's characteristics. For example,
// trigrams and scopes.
// FIXME(kbobyrev): Support more token types:
// * Path proximity
// * Types
std::vector<Token> generateSearchTokens(const Symbol &Sym) {
  std::vector<Token> Result = generateIdentifierTrigrams(Sym.Name);
  Result.push_back(Token(Token::Kind::Scope, Sym.Scope));
  return Result;
}

} // namespace

void DexIndex::build(std::shared_ptr<std::vector<const Symbol *>> Syms) {
  llvm::DenseMap<SymbolID, const Symbol *> TempLookupTable;
  llvm::DenseMap<const Symbol *, float> TempSymbolQuality;
  for (const Symbol *Sym : *Syms) {
    TempLookupTable[Sym->ID] = Sym;
    TempSymbolQuality[Sym] = quality(*Sym);
  }

  // Symbols are sorted by symbol qualities so that items in the posting lists
  // are stored in the descending order of symbol quality.
  std::sort(begin(*Syms), end(*Syms),
            [&](const Symbol *LHS, const Symbol *RHS) {
              return TempSymbolQuality[LHS] > TempSymbolQuality[RHS];
            });
  llvm::DenseMap<Token, PostingList> TempInvertedIndex;
  // Populate TempInvertedIndex with posting lists for index symbols.
  for (DocID SymbolRank = 0; SymbolRank < Syms->size(); ++SymbolRank) {
    const auto *Sym = (*Syms)[SymbolRank];
    for (const auto &Token : generateSearchTokens(*Sym))
      TempInvertedIndex[Token].push_back(SymbolRank);
  }

  {
    std::lock_guard<std::mutex> Lock(Mutex);

    // Replace outdated index with the new one.
    LookupTable = std::move(TempLookupTable);
    Symbols = std::move(Syms);
    InvertedIndex = std::move(TempInvertedIndex);
    SymbolQuality = std::move(TempSymbolQuality);
  }

  vlog("Built DexIndex with estimated memory usage {0} bytes.",
       estimateMemoryUsage());
}

std::unique_ptr<SymbolIndex> DexIndex::build(SymbolSlab Slab) {
  auto Idx = llvm::make_unique<DexIndex>();
  Idx->build(getSymbolsFromSlab(std::move(Slab)));
  return std::move(Idx);
}

/// Constructs iterators over tokens extracted from the query and exhausts it
/// while applying Callback to each symbol in the order of decreasing quality
/// of the matched symbols.
bool DexIndex::fuzzyFind(
    const FuzzyFindRequest &Req,
    llvm::function_ref<void(const Symbol &)> Callback) const {
  assert(!StringRef(Req.Query).contains("::") &&
         "There must be no :: in query.");
  FuzzyMatcher Filter(Req.Query);
  bool More = false;

  std::vector<std::unique_ptr<Iterator>> TopLevelChildren;
  const auto TrigramTokens = generateIdentifierTrigrams(Req.Query);

  {
    std::lock_guard<std::mutex> Lock(Mutex);

    // Generate query trigrams and construct AND iterator over all query
    // trigrams.
    std::vector<std::unique_ptr<Iterator>> TrigramIterators;
    for (const auto &Trigram : TrigramTokens) {
      const auto It = InvertedIndex.find(Trigram);
      if (It != InvertedIndex.end())
        TrigramIterators.push_back(create(It->second));
    }
    if (!TrigramIterators.empty())
      TopLevelChildren.push_back(createAnd(move(TrigramIterators)));

    // Generate scope tokens for search query.
    std::vector<std::unique_ptr<Iterator>> ScopeIterators;
    for (const auto &Scope : Req.Scopes) {
      const auto It = InvertedIndex.find(Token(Token::Kind::Scope, Scope));
      if (It != InvertedIndex.end())
        ScopeIterators.push_back(create(It->second));
    }
    // Add OR iterator for scopes if there are any Scope Iterators.
    if (!ScopeIterators.empty())
      TopLevelChildren.push_back(createOr(move(ScopeIterators)));

    // Use TRUE iterator if both trigrams and scopes from the query are not
    // present in the symbol index.
    auto QueryIterator = TopLevelChildren.empty()
                             ? createTrue(Symbols->size())
                             : createAnd(move(TopLevelChildren));
    // Retrieve more items than it was requested: some of  the items with high
    // final score might not be retrieved otherwise.
    // FIXME(kbobyrev): Pre-scoring retrieval threshold should be adjusted as
    // using 100x of the requested number might not be good in practice, e.g.
    // when the requested number of items is small.
    const unsigned ItemsToRetrieve = 100 * Req.MaxCandidateCount;
    // FIXME(kbobyrev): Add boosting to the query and utilize retrieved
    // boosting scores.
    std::vector<std::pair<DocID, float>> SymbolDocIDs =
        consume(*QueryIterator, ItemsToRetrieve);

    // Retrieve top Req.MaxCandidateCount items.
    std::priority_queue<std::pair<float, const Symbol *>> Top;
    for (const auto &P : SymbolDocIDs) {
      const DocID SymbolDocID = P.first;
      const auto *Sym = (*Symbols)[SymbolDocID];
      const llvm::Optional<float> Score = Filter.match(Sym->Name);
      if (!Score)
        continue;
      // Multiply score by a negative factor so that Top stores items with the
      // highest actual score.
      Top.emplace(-(*Score) * SymbolQuality.find(Sym)->second, Sym);
      if (Top.size() > Req.MaxCandidateCount) {
        More = true;
        Top.pop();
      }
    }

    // Apply callback to the top Req.MaxCandidateCount items.
    for (; !Top.empty(); Top.pop())
      Callback(*Top.top().second);
  }

  return More;
}

void DexIndex::lookup(const LookupRequest &Req,
                      llvm::function_ref<void(const Symbol &)> Callback) const {
  std::lock_guard<std::mutex> Lock(Mutex);
  for (const auto &ID : Req.IDs) {
    auto I = LookupTable.find(ID);
    if (I != LookupTable.end())
      Callback(*I->second);
  }
}

void DexIndex::findOccurrences(
    const OccurrencesRequest &Req,
    llvm::function_ref<void(const SymbolOccurrence &)> Callback) const {
  log("findOccurrences is not implemented.");
}

size_t DexIndex::estimateMemoryUsage() const {
  std::lock_guard<std::mutex> Lock(Mutex);

  size_t Bytes =
      LookupTable.size() * sizeof(std::pair<SymbolID, const Symbol *>);
  Bytes += SymbolQuality.size() * sizeof(std::pair<const Symbol *, float>);
  Bytes += InvertedIndex.size() * sizeof(Token);

  for (const auto &P : InvertedIndex) {
    Bytes += P.second.size() * sizeof(DocID);
  }
  return Bytes;
}

} // namespace dex
} // namespace clangd
} // namespace clang
