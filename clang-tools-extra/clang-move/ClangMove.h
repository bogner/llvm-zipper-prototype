//===-- ClangMove.h - Clang move  -----------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_MOVE_CLANGMOVE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_MOVE_CLANGMOVE_H

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace clang {
namespace move {

// A reporter which collects and reports declarations in old header.
class DeclarationReporter {
public:
  DeclarationReporter() = default;
  ~DeclarationReporter() = default;

  void reportDeclaration(llvm::StringRef DeclarationName,
                         llvm::StringRef Type) {
    DeclarationList.emplace_back(DeclarationName, Type);
  };

  // A <DeclarationName, DeclarationKind> pair.
  // The DeclarationName is a fully qualified name for the declaration, like
  // A::B::Foo. The DeclarationKind is a string represents the kind of the
  // declaration, currently only "Function" and "Class" are supported.
  typedef std::pair<std::string, std::string> DeclarationPair;

  const std::vector<DeclarationPair> getDeclarationList() const {
    return DeclarationList;
  }

private:
  std::vector<DeclarationPair> DeclarationList;
};

// Specify declarations being moved. It contains all information of the moved
// declarations.
struct MoveDefinitionSpec {
  // The list of fully qualified names, e.g. Foo, a::Foo, b::Foo.
  SmallVector<std::string, 4> Names;
  // The file path of old header, can be relative path and absolute path.
  std::string OldHeader;
  // The file path of old cc, can be relative path and absolute path.
  std::string OldCC;
  // The file path of new header, can be relative path and absolute path.
  std::string NewHeader;
  // The file path of new cc, can be relative path and absolute path.
  std::string NewCC;
  // Whether old.h depends on new.h. If true, #include "new.h" will be added
  // in old.h.
  bool OldDependOnNew = false;
  // Whether new.h depends on old.h. If true, #include "old.h" will be added
  // in new.h.
  bool NewDependOnOld = false;
};

// A Context which contains extra options which are used in ClangMoveTool.
struct ClangMoveContext {
  MoveDefinitionSpec Spec;
  // The Key is file path, value is the replacements being applied to the file.
  std::map<std::string, tooling::Replacements> &FileToReplacements;
  // The original working directory where the local clang-move binary runs.
  //
  // clang-move will change its current working directory to the build
  // directory when analyzing the source file. We save the original working
  // directory in order to get the absolute file path for the fields in Spec.
  std::string OriginalRunningDirectory;
  // The name of a predefined code style.
  std::string FallbackStyle;
  // Whether dump all declarations in old header.
  bool DumpDeclarations;
};

// This tool is used to move class/function definitions from the given source
// files (old.h/cc) to new files (new.h/cc). When moving a class, all its
// members are also moved. In addition, all helper functions (anonymous
// namespace declarations, static declarations, using declarations) in old.cc
// and forward class declarations in old.h are copied to the new files.
// The goal of this tool is to make the new files as compliable as possible.
//
// Note: When all declarations in old header are being moved, all code in
// old.h/cc will be moved, which means old.h/cc are empty.
class ClangMoveTool : public ast_matchers::MatchFinder::MatchCallback {
public:
  // Information about the declaration being moved.
  struct MovedDecl {
    // FIXME: Replace Decl with SourceRange to get rid of calculating range for
    // the Decl duplicately.
    const clang::NamedDecl *Decl = nullptr;
    clang::SourceManager *SM = nullptr;
    MovedDecl() = default;
    MovedDecl(const clang::NamedDecl *Decl, clang::SourceManager *SM)
        : Decl(Decl), SM(SM) {}
  };

  ClangMoveTool(ClangMoveContext *const Context,
                DeclarationReporter *const Reporter);

  void registerMatchers(ast_matchers::MatchFinder *Finder);

  void run(const ast_matchers::MatchFinder::MatchResult &Result) override;

  void onEndOfTranslationUnit() override;

  /// Add #includes from old.h/cc files.
  ///
  /// \param IncludeHeader The name of the file being included, as written in
  /// the source code.
  /// \param IsAngled Whether the file name was enclosed in angle brackets.
  /// \param SearchPath The search path which was used to find the IncludeHeader
  /// in the file system. It can be a relative path or an absolute path.
  /// \param FileName The name of file where the IncludeHeader comes from.
  /// \param IncludeFilenameRange The source range for the written file name in
  /// #include (i.e. "old.h" for #include "old.h") in old.cc.
  /// \param SM The SourceManager.
  void addIncludes(llvm::StringRef IncludeHeader, bool IsAngled,
                   llvm::StringRef SearchPath, llvm::StringRef FileName,
                   clang::CharSourceRange IncludeFilenameRange,
                   const SourceManager &SM);

  std::vector<MovedDecl> &getMovedDecls() { return MovedDecls; }

  /// Add declarations being removed from old.h/cc. For each declarations, the
  /// method also records the mapping relationship between the corresponding
  /// FilePath and its FileID.
  void addRemovedDecl(const MovedDecl &Decl);

  llvm::SmallPtrSet<const NamedDecl *, 8> &getUnremovedDeclsInOldHeader() {
    return UnremovedDeclsInOldHeader;
  }

private:
  // Make the Path absolute using the OrignalRunningDirectory if the Path is not
  // an absolute path. An empty Path will result in an empty string.
  std::string makeAbsolutePath(StringRef Path);

  void removeClassDefinitionInOldFiles();
  void moveClassDefinitionToNewFiles();
  void moveAll(SourceManager& SM, StringRef OldFile, StringRef NewFile);

  // Stores all MatchCallbacks created by this tool.
  std::vector<std::unique_ptr<ast_matchers::MatchFinder::MatchCallback>>
      MatchCallbacks;
  // All declarations (the class decl being moved, forward decls) that need to
  // be moved/copy to the new files, saving in an AST-visited order.
  std::vector<MovedDecl> MovedDecls;
  // The declarations that needs to be removed in old.cc/h.
  std::vector<MovedDecl> RemovedDecls;
  // The #includes in old_header.h.
  std::vector<std::string> HeaderIncludes;
  // The #includes in old_cc.cc.
  std::vector<std::string> CCIncludes;
  // The unmoved named declarations in old header.
  llvm::SmallPtrSet<const NamedDecl*, 8> UnremovedDeclsInOldHeader;
  /// The source range for the written file name in #include (i.e. "old.h" for
  /// #include "old.h") in old.cc,  including the enclosing quotes or angle
  /// brackets.
  clang::CharSourceRange OldHeaderIncludeRange;
  /// Mapping from FilePath to FileID, which can be used in post processes like
  /// cleanup around replacements.
  llvm::StringMap<FileID> FilePathToFileID;
  /// A context contains all running options. It is not owned.
  ClangMoveContext *const Context;
  /// A reporter to report all declarations from old header. It is not owned.
  DeclarationReporter *const Reporter;
};

class ClangMoveAction : public clang::ASTFrontendAction {
public:
  ClangMoveAction(ClangMoveContext *const Context,
                  DeclarationReporter *const Reporter)
      : MoveTool(Context, Reporter) {
    MoveTool.registerMatchers(&MatchFinder);
  }

  ~ClangMoveAction() override = default;

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &Compiler,
                    llvm::StringRef InFile) override;

private:
  ast_matchers::MatchFinder MatchFinder;
  ClangMoveTool MoveTool;
};

class ClangMoveActionFactory : public tooling::FrontendActionFactory {
public:
  ClangMoveActionFactory(ClangMoveContext *const Context,
                         DeclarationReporter *const Reporter = nullptr)
      : Context(Context), Reporter(Reporter) {}

  clang::FrontendAction *create() override {
    return new ClangMoveAction(Context, Reporter);
  }

private:
  // Not owned.
  ClangMoveContext *const Context;
  DeclarationReporter *const Reporter;
};

} // namespace move
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_MOVE_CLANGMOVE_H
