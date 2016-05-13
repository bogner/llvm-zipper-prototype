//===-- SymbolIndexManager.cpp - Managing multiple SymbolIndices-*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SymbolIndexManager.h"
#include "find-all-symbols/SymbolInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "include-fixer"

namespace clang {
namespace include_fixer {

std::vector<std::string>
SymbolIndexManager::search(llvm::StringRef Identifier) const {
  // The identifier may be fully qualified, so split it and get all the context
  // names.
  llvm::SmallVector<llvm::StringRef, 8> Names;
  Identifier.split(Names, "::");

  std::vector<clang::find_all_symbols::SymbolInfo> Symbols;
  for (const auto &DB : SymbolIndices) {
    auto Res = DB->search(Names.back().str());
    Symbols.insert(Symbols.end(), Res.begin(), Res.end());
  }

  DEBUG(llvm::dbgs() << "Searching " << Names.back() << "... got "
                     << Symbols.size() << " results...\n");

  std::vector<std::string> Results;
  for (const auto &Symbol : Symbols) {
    // Match the identifier name without qualifier.
    if (Symbol.getName() == Names.back()) {
      bool IsMatched = true;
      auto SymbolContext = Symbol.getContexts().begin();
      // Match the remaining context names.
      for (auto IdentiferContext = Names.rbegin() + 1;
           IdentiferContext != Names.rend() &&
           SymbolContext != Symbol.getContexts().end();
           ++IdentiferContext, ++SymbolContext) {
        if (SymbolContext->second != *IdentiferContext) {
          IsMatched = false;
          break;
        }
      }

      if (IsMatched) {
        // FIXME: file path should never be in the form of <...> or "...", but
        // the unit test with fixed database use <...> file path, which might
        // need to be changed.
        // FIXME: if the file path is a system header name, we want to use angle
        // brackets.
        std::string FilePath = Symbol.getFilePath().str();
        Results.push_back((FilePath[0] == '"' || FilePath[0] == '<')
                              ? FilePath
                              : "\"" + FilePath + "\"");
      }
    }
  }
  return Results;
}

} // namespace include_fixer
} // namespace clang
