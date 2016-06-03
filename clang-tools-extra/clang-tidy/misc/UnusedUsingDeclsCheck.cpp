//===--- UnusedUsingDeclsCheck.cpp - clang-tidy----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "UnusedUsingDeclsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace misc {

// A function that helps to tell whether a TargetDecl in a UsingDecl will be
// checked. Only variable, function, function template, class template and class
// are considered.
static bool ShouldCheckDecl(const Decl *TargetDecl) {
  return isa<RecordDecl>(TargetDecl) || isa<ClassTemplateDecl>(TargetDecl) ||
         isa<FunctionDecl>(TargetDecl) || isa<VarDecl>(TargetDecl) ||
         isa<FunctionTemplateDecl>(TargetDecl);
}

void UnusedUsingDeclsCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(usingDecl(isExpansionInMainFile()).bind("using"), this);
  auto DeclMatcher = hasDeclaration(namedDecl().bind("used"));
  Finder->addMatcher(loc(recordType(DeclMatcher)), this);
  Finder->addMatcher(loc(templateSpecializationType(DeclMatcher)), this);
  Finder->addMatcher(declRefExpr().bind("used"), this);
  Finder->addMatcher(callExpr(callee(unresolvedLookupExpr().bind("used"))),
                     this);
}

void UnusedUsingDeclsCheck::check(const MatchFinder::MatchResult &Result) {
  if (const auto *Using = Result.Nodes.getNodeAs<UsingDecl>("using")) {
    // Ignores using-declarations defined in macros.
    if (Using->getLocation().isMacroID())
      return;

    // Ignores using-declarations defined in class definition.
    if (isa<CXXRecordDecl>(Using->getDeclContext()))
      return;

    // FIXME: We ignore using-decls defined in function definitions at the
    // moment because of false positives caused by ADL and different function
    // scopes.
    if (isa<FunctionDecl>(Using->getDeclContext()))
      return;

    UsingDeclContext Context(Using);
    Context.UsingDeclRange = CharSourceRange::getCharRange(
        Using->getLocStart(),
        Lexer::findLocationAfterToken(
            Using->getLocEnd(), tok::semi, *Result.SourceManager,
            Result.Context->getLangOpts(),
            /*SkipTrailingWhitespaceAndNewLine=*/true));
    for (const auto *UsingShadow : Using->shadows()) {
      const auto *TargetDecl = UsingShadow->getTargetDecl()->getCanonicalDecl();
      if (ShouldCheckDecl(TargetDecl))
        Context.UsingTargetDecls.insert(TargetDecl);
    }
    if (!Context.UsingTargetDecls.empty())
      Contexts.push_back(Context);
    return;
  }

  // Mark using declarations as used by setting FoundDecls' value to zero. As
  // the AST is walked in order, usages are only marked after a the
  // corresponding using declaration has been found.
  // FIXME: This currently doesn't look at whether the type reference is
  // actually found with the help of the using declaration.
  if (const auto *Used = Result.Nodes.getNodeAs<NamedDecl>("used")) {
    if (const auto *Specialization =
            dyn_cast<ClassTemplateSpecializationDecl>(Used))
      Used = Specialization->getSpecializedTemplate();
    removeFromFoundDecls(Used);
    return;
  }

  if (const auto *DRE = Result.Nodes.getNodeAs<DeclRefExpr>("used")) {
    if (const auto *FD = dyn_cast<FunctionDecl>(DRE->getDecl())) {
      if (const auto *FDT = FD->getPrimaryTemplate())
        removeFromFoundDecls(FDT);
      else
        removeFromFoundDecls(FD);
    } else if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      removeFromFoundDecls(VD);
    }
  }
  // Check the uninstantiated template function usage.
  if (const auto *ULE = Result.Nodes.getNodeAs<UnresolvedLookupExpr>("used")) {
    for (const NamedDecl* ND : ULE->decls()) {
      if (const auto *USD = dyn_cast<UsingShadowDecl>(ND))
        removeFromFoundDecls(USD->getTargetDecl()->getCanonicalDecl());
    }
  }
}

void UnusedUsingDeclsCheck::removeFromFoundDecls(const Decl *D) {
  // FIXME: Currently, we don't handle the using-decls being used in different
  // scopes (such as different namespaces, different functions). Instead of
  // giving an incorrect message, we mark all of them as used.
  //
  // FIXME: Use a more efficient way to find a matching context.
  for (auto &Context : Contexts) {
    if (Context.UsingTargetDecls.count(D->getCanonicalDecl()) > 0)
      Context.IsUsed = true;
  }
}

void UnusedUsingDeclsCheck::onEndOfTranslationUnit() {
  for (const auto &Context : Contexts) {
    if (!Context.IsUsed) {
      diag(Context.FoundUsingDecl->getLocation(), "using decl %0 is unused")
          << Context.FoundUsingDecl
          << FixItHint::CreateRemoval(Context.UsingDeclRange);
    }
  }
  Contexts.clear();
}

} // namespace misc
} // namespace tidy
} // namespace clang
