//===--- Quality.cpp --------------------------------------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
#include "Quality.h"
#include "index/Index.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {
namespace clangd {
using namespace llvm;

static bool hasDeclInMainFile(const Decl &D) {
  auto &SourceMgr = D.getASTContext().getSourceManager();
  for (auto *Redecl : D.redecls()) {
    auto Loc = SourceMgr.getSpellingLoc(Redecl->getLocation());
    if (SourceMgr.isWrittenInMainFile(Loc))
      return true;
  }
  return false;
}

void SymbolQualitySignals::merge(const CodeCompletionResult &SemaCCResult) {
  SemaCCPriority = SemaCCResult.Priority;
  if (SemaCCResult.Availability == CXAvailability_Deprecated)
    Deprecated = true;
}

void SymbolQualitySignals::merge(const Symbol &IndexResult) {
  References = std::max(IndexResult.References, References);
}

float SymbolQualitySignals::evaluate() const {
  float Score = 1;

  // This avoids a sharp gradient for tail symbols, and also neatly avoids the
  // question of whether 0 references means a bad symbol or missing data.
  if (References >= 3)
    Score *= std::log(References);

  if (SemaCCPriority)
    // Map onto a 0-2 interval, so we don't reward/penalize non-Sema results.
    // Priority 80 is a really bad score.
    Score *= 2 - std::min<float>(80, SemaCCPriority) / 40;

  if (Deprecated)
    Score *= 0.1f;

  return Score;
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolQualitySignals &S) {
  OS << formatv("=== Symbol quality: {0}\n", S.evaluate());
  if (S.SemaCCPriority)
    OS << formatv("\tSemaCCPriority: {0}\n", S.SemaCCPriority);
  OS << formatv("\tReferences: {0}\n", S.References);
  OS << formatv("\tDeprecated: {0}\n", S.Deprecated);
  return OS;
}

static SymbolRelevanceSignals::AccessibleScope
ComputeScope(const NamedDecl &D) {
  bool InClass;
  for (const DeclContext *DC = D.getDeclContext(); !DC->isFileContext();
       DC = DC->getParent()) {
    if (DC->isFunctionOrMethod())
      return SymbolRelevanceSignals::FunctionScope;
    InClass = InClass || DC->isRecord();
  }
  if (InClass)
    return SymbolRelevanceSignals::ClassScope;
  // This threshold could be tweaked, e.g. to treat module-visible as global.
  if (D.getLinkageInternal() < ExternalLinkage)
    return SymbolRelevanceSignals::FileScope;
  return SymbolRelevanceSignals::GlobalScope;
}

void SymbolRelevanceSignals::merge(const Symbol &IndexResult) {
  // FIXME: Index results always assumed to be at global scope. If Scope becomes
  // relevant to non-completion requests, we should recognize class members etc.
}

void SymbolRelevanceSignals::merge(const CodeCompletionResult &SemaCCResult) {
  if (SemaCCResult.Availability == CXAvailability_NotAvailable ||
      SemaCCResult.Availability == CXAvailability_NotAccessible)
    Forbidden = true;

  if (SemaCCResult.Declaration) {
    // We boost things that have decls in the main file.
    // The real proximity scores would be more general when we have them.
    float DeclProximity =
        hasDeclInMainFile(*SemaCCResult.Declaration) ? 1.0 : 0.0;
    ProximityScore = std::max(DeclProximity, ProximityScore);
  }

  // Declarations are scoped, others (like macros) are assumed global.
  if (SemaCCResult.Kind == CodeCompletionResult::RK_Declaration)
    Scope = std::min(Scope, ComputeScope(*SemaCCResult.Declaration));
}

float SymbolRelevanceSignals::evaluate() const {
  float Score = 1;

  if (Forbidden)
    return 0;

  Score *= NameMatch;

  // Proximity scores are [0,1] and we translate them into a multiplier in the
  // range from 1 to 2.
  Score *= 1 + ProximityScore;

  // Symbols like local variables may only be referenced within their scope.
  // Conversely if we're in that scope, it's likely we'll reference them.
  if (Query == CodeComplete) {
    // The narrower the scope where a symbol is visible, the more likely it is
    // to be relevant when it is available.
    switch (Scope) {
    case GlobalScope:
      break;
    case FileScope:
      Score *= 1.5;
    case ClassScope:
      Score *= 2;
    case FunctionScope:
      Score *= 4;
    }
  }

  return Score;
}
raw_ostream &operator<<(raw_ostream &OS, const SymbolRelevanceSignals &S) {
  OS << formatv("=== Symbol relevance: {0}\n", S.evaluate());
  OS << formatv("\tName match: {0}\n", S.NameMatch);
  OS << formatv("\tForbidden: {0}\n", S.Forbidden);
  return OS;
}

float evaluateSymbolAndRelevance(float SymbolQuality, float SymbolRelevance) {
  return SymbolQuality * SymbolRelevance;
}

// Produces an integer that sorts in the same order as F.
// That is: a < b <==> encodeFloat(a) < encodeFloat(b).
static uint32_t encodeFloat(float F) {
  static_assert(std::numeric_limits<float>::is_iec559, "");
  constexpr uint32_t TopBit = ~(~uint32_t{0} >> 1);

  // Get the bits of the float. Endianness is the same as for integers.
  uint32_t U = FloatToBits(F);
  // IEEE 754 floats compare like sign-magnitude integers.
  if (U & TopBit)    // Negative float.
    return 0 - U;    // Map onto the low half of integers, order reversed.
  return U + TopBit; // Positive floats map onto the high half of integers.
}

std::string sortText(float Score, llvm::StringRef Name) {
  // We convert -Score to an integer, and hex-encode for readability.
  // Example: [0.5, "foo"] -> "41000000foo"
  std::string S;
  llvm::raw_string_ostream OS(S);
  write_hex(OS, encodeFloat(-Score), llvm::HexPrintStyle::Lower,
            /*Width=*/2 * sizeof(Score));
  OS << Name;
  OS.flush();
  return S;
}

} // namespace clangd
} // namespace clang
