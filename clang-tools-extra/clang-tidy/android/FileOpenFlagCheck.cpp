//===--- FileOpenFlagCheck.cpp - clang-tidy--------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "FileOpenFlagCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace android {

namespace {
static constexpr const char *O_CLOEXEC = "O_CLOEXEC";

bool HasCloseOnExecFlag(const Expr *Flags, const SourceManager &SM,
                        const LangOptions &LangOpts) {
  // If the Flag is an integer constant, check it.
  if (isa<IntegerLiteral>(Flags)) {
    if (!SM.isMacroBodyExpansion(Flags->getLocStart()))
      return false;

    // Get the Marco name.
    auto MacroName = Lexer::getSourceText(
        CharSourceRange::getTokenRange(Flags->getSourceRange()), SM, LangOpts);

    return MacroName == O_CLOEXEC;
  }
  // If it's a binary OR operation.
  if (const auto *BO = dyn_cast<BinaryOperator>(Flags))
    if (BO->getOpcode() == clang::BinaryOperatorKind::BO_Or)
      return HasCloseOnExecFlag(BO->getLHS()->IgnoreParenCasts(), SM,
                                LangOpts) ||
             HasCloseOnExecFlag(BO->getRHS()->IgnoreParenCasts(), SM, LangOpts);

  // Otherwise, assume it has the flag.
  return true;
}
} // namespace

void FileOpenFlagCheck::registerMatchers(MatchFinder *Finder) {
  auto CharPointerType = hasType(pointerType(pointee(isAnyCharacter())));

  Finder->addMatcher(
      callExpr(callee(functionDecl(isExternC(), returns(isInteger()),
                                   hasAnyName("open", "open64"),
                                   hasParameter(0, CharPointerType),
                                   hasParameter(1, hasType(isInteger())))
                          .bind("funcDecl")))
          .bind("openFn"),
      this);
  Finder->addMatcher(
      callExpr(callee(functionDecl(isExternC(), returns(isInteger()),
                                   hasName("openat"),
                                   hasParameter(0, hasType(isInteger())),
                                   hasParameter(1, CharPointerType),
                                   hasParameter(2, hasType(isInteger())))
                          .bind("funcDecl")))
          .bind("openatFn"),
      this);
}

void FileOpenFlagCheck::check(const MatchFinder::MatchResult &Result) {
  const Expr *FlagArg = nullptr;
  if (const auto *OpenFnCall = Result.Nodes.getNodeAs<CallExpr>("openFn"))
    FlagArg = OpenFnCall->getArg(1);
  else if (const auto *OpenFnCall =
               Result.Nodes.getNodeAs<CallExpr>("openatFn"))
    FlagArg = OpenFnCall->getArg(2);
  assert(FlagArg);

  const auto *FD = Result.Nodes.getNodeAs<FunctionDecl>("funcDecl");

  // Check the required flag.
  SourceManager &SM = *Result.SourceManager;
  if (HasCloseOnExecFlag(FlagArg->IgnoreParenCasts(), SM,
                         Result.Context->getLangOpts()))
    return;

  SourceLocation EndLoc = Lexer::getLocForEndOfToken(
      FlagArg->getLocEnd(), 0, SM, Result.Context->getLangOpts());

  diag(EndLoc, "%0 should use %1 where possible")
      << FD << O_CLOEXEC
      << FixItHint::CreateInsertion(EndLoc, (Twine(" | ") + O_CLOEXEC).str());
}

} // namespace android
} // namespace tidy
} // namespace clang
