//===--- tools/extra/clang-rename/USRFinder.cpp - Clang rename tool -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file Implements a recursive AST visitor that finds the USR of a symbol at a
/// point.
///
//===----------------------------------------------------------------------===//

#include "USRFinder.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/SmallVector.h"

using namespace llvm;

namespace clang {
namespace rename {

// NamedDeclFindingASTVisitor recursively visits each AST node to find the
// symbol underneath the cursor.
// FIXME: move to seperate .h/.cc file if this gets too large.
namespace {
class NamedDeclFindingASTVisitor
    : public clang::RecursiveASTVisitor<NamedDeclFindingASTVisitor> {
public:
  // \brief Finds the NamedDecl at a point in the source.
  // \param Point the location in the source to search for the NamedDecl.
  explicit NamedDeclFindingASTVisitor(const SourceManager &SourceMgr,
                                      const SourceLocation Point,
                                      const ASTContext *Context)
      : Result(nullptr), SourceMgr(SourceMgr), Point(Point), Context(Context) {}

  // \brief Finds the NamedDecl for a name in the source.
  // \param Name the fully qualified name.
  explicit NamedDeclFindingASTVisitor(const SourceManager &SourceMgr,
                                      const std::string &Name,
                                      const ASTContext *Context)
      : Result(nullptr), SourceMgr(SourceMgr), Name(Name), Context(Context) {}

  // Declaration visitors:

  // \brief Checks if the point falls within the NameDecl. This covers every
  // declaration of a named entity that we may come across. Usually, just
  // checking if the point lies within the length of the name of the declaration
  // and the start location is sufficient.
  bool VisitNamedDecl(const NamedDecl *Decl) {
    return dyn_cast<CXXConversionDecl>(Decl) ? true :
        setResult(Decl, Decl->getLocation(), Decl->getNameAsString().length());
  }

  // Expression visitors:

  bool VisitDeclRefExpr(const DeclRefExpr *Expr) {
    const auto *Decl = Expr->getFoundDecl();
    return setResult(Decl, Expr->getLocation(),
                     Decl->getNameAsString().length());
  }

  bool VisitMemberExpr(const MemberExpr *Expr) {
    const auto *Decl = Expr->getFoundDecl().getDecl();
    return setResult(Decl, Expr->getMemberLoc(),
                     Decl->getNameAsString().length());
  }

  // Other visitors:

  bool VisitTypeLoc(const TypeLoc Loc) {
    const auto TypeBeginLoc = Loc.getBeginLoc();
    const auto TypeEndLoc = Lexer::getLocForEndOfToken(
        TypeBeginLoc, 0, SourceMgr, Context->getLangOpts());
    if (auto *RD = Loc.getType()->getAsCXXRecordDecl())
      return setResult(RD, TypeBeginLoc, TypeEndLoc);
    return true;
  }

  // Other:

  const NamedDecl *getNamedDecl() { return Result; }

  // \brief Determines if a namespace qualifier contains the point.
  // \returns false on success and sets Result.
  void handleNestedNameSpecifierLoc(NestedNameSpecifierLoc NameLoc) {
    while (NameLoc) {
      const auto *Decl = NameLoc.getNestedNameSpecifier()->getAsNamespace();
      if (Decl) {
        setResult(Decl, NameLoc.getLocalBeginLoc(), NameLoc.getLocalEndLoc());
      }
      NameLoc = NameLoc.getPrefix();
    }
  }

private:
  // \brief Sets Result to Decl if the Point is within Start and End.
  // \returns false on success.
  bool setResult(const NamedDecl *Decl, SourceLocation Start,
                 SourceLocation End) {
    if (Name.empty()) {
      // Offset is used to find the declaration.
      if (!Start.isValid() || !Start.isFileID() || !End.isValid() ||
          !End.isFileID() || !isPointWithin(Start, End)) {
        return true;
      }
    } else {
      // Fully qualified name is used to find the declaration.
      if (Name != Decl->getQualifiedNameAsString()) {
        return true;
      }
    }
    Result = Decl;
    return false;
  }

  // \brief Sets Result to Decl if Point is within Loc and Loc + Offset.
  // \returns false on success.
  bool setResult(const NamedDecl *Decl, SourceLocation Loc, unsigned Offset) {
    // FIXME: Add test for Offset == 0. Add test for Offset - 1 (vs -2 etc).
    return Offset == 0 ||
           setResult(Decl, Loc, Loc.getLocWithOffset(Offset - 1));
  }

  // \brief Determines if the Point is within Start and End.
  bool isPointWithin(const SourceLocation Start, const SourceLocation End) {
    // FIXME: Add tests for Point == End.
    return Point == Start || Point == End ||
           (SourceMgr.isBeforeInTranslationUnit(Start, Point) &&
            SourceMgr.isBeforeInTranslationUnit(Point, End));
  }

  const NamedDecl *Result;
  const SourceManager &SourceMgr;
  const SourceLocation Point; // The location to find the NamedDecl.
  const std::string Name;
  const ASTContext *Context;
};
} // namespace

const NamedDecl *getNamedDeclAt(const ASTContext &Context,
                                const SourceLocation Point) {
  const auto &SourceMgr = Context.getSourceManager();
  const auto SearchFile = SourceMgr.getFilename(Point);

  NamedDeclFindingASTVisitor Visitor(SourceMgr, Point, &Context);

  // We only want to search the decls that exist in the same file as the point.
  auto Decls = Context.getTranslationUnitDecl()->decls();
  for (auto &CurrDecl : Decls) {
    const auto FileLoc = CurrDecl->getLocStart();
    const auto FileName = SourceMgr.getFilename(FileLoc);
    // FIXME: Add test.
    if (FileName == SearchFile) {
      Visitor.TraverseDecl(CurrDecl);
    }
  }

  NestedNameSpecifierLocFinder Finder(const_cast<ASTContext &>(Context));
  for (const auto &Location : Finder.getNestedNameSpecifierLocations()) {
    Visitor.handleNestedNameSpecifierLoc(Location);
  }

  return Visitor.getNamedDecl();
}

const NamedDecl *getNamedDeclFor(const ASTContext &Context,
                                 const std::string &Name) {
  const auto &SourceMgr = Context.getSourceManager();
  NamedDeclFindingASTVisitor Visitor(SourceMgr, Name, &Context);
  Visitor.TraverseDecl(Context.getTranslationUnitDecl());

  return Visitor.getNamedDecl();
}

std::string getUSRForDecl(const Decl *Decl) {
  llvm::SmallVector<char, 128> Buff;

  // FIXME: Add test for the nullptr case.
  if (Decl == nullptr || index::generateUSRForDecl(Decl, Buff))
    return "";

  return std::string(Buff.data(), Buff.size());
}

} // namespace rename
} // namespace clang
