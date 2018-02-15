//===--- ClangdUnit.cpp -----------------------------------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//

#include "ClangdUnit.h"
#include "Compiler.h"
#include "Logger.h"
#include "Trace.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/Utils.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace clang::clangd;
using namespace clang;

namespace {

template <class T> std::size_t getUsedBytes(const std::vector<T> &Vec) {
  return Vec.capacity() * sizeof(T);
}

class DeclTrackingASTConsumer : public ASTConsumer {
public:
  DeclTrackingASTConsumer(std::vector<const Decl *> &TopLevelDecls)
      : TopLevelDecls(TopLevelDecls) {}

  bool HandleTopLevelDecl(DeclGroupRef DG) override {
    for (const Decl *D : DG) {
      // ObjCMethodDecl are not actually top-level decls.
      if (isa<ObjCMethodDecl>(D))
        continue;

      TopLevelDecls.push_back(D);
    }
    return true;
  }

private:
  std::vector<const Decl *> &TopLevelDecls;
};

class ClangdFrontendAction : public SyntaxOnlyAction {
public:
  std::vector<const Decl *> takeTopLevelDecls() {
    return std::move(TopLevelDecls);
  }

protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override {
    return llvm::make_unique<DeclTrackingASTConsumer>(/*ref*/ TopLevelDecls);
  }

private:
  std::vector<const Decl *> TopLevelDecls;
};

class CppFilePreambleCallbacks : public PreambleCallbacks {
public:
  std::vector<serialization::DeclID> takeTopLevelDeclIDs() {
    return std::move(TopLevelDeclIDs);
  }

  void AfterPCHEmitted(ASTWriter &Writer) override {
    TopLevelDeclIDs.reserve(TopLevelDecls.size());
    for (Decl *D : TopLevelDecls) {
      // Invalid top-level decls may not have been serialized.
      if (D->isInvalidDecl())
        continue;
      TopLevelDeclIDs.push_back(Writer.getDeclID(D));
    }
  }

  void HandleTopLevelDecl(DeclGroupRef DG) override {
    for (Decl *D : DG) {
      if (isa<ObjCMethodDecl>(D))
        continue;
      TopLevelDecls.push_back(D);
    }
  }

private:
  std::vector<Decl *> TopLevelDecls;
  std::vector<serialization::DeclID> TopLevelDeclIDs;
};

/// Convert from clang diagnostic level to LSP severity.
static int getSeverity(DiagnosticsEngine::Level L) {
  switch (L) {
  case DiagnosticsEngine::Remark:
    return 4;
  case DiagnosticsEngine::Note:
    return 3;
  case DiagnosticsEngine::Warning:
    return 2;
  case DiagnosticsEngine::Fatal:
  case DiagnosticsEngine::Error:
    return 1;
  case DiagnosticsEngine::Ignored:
    return 0;
  }
  llvm_unreachable("Unknown diagnostic level!");
}

// Checks whether a location is within a half-open range.
// Note that clang also uses closed source ranges, which this can't handle!
bool locationInRange(SourceLocation L, CharSourceRange R,
                     const SourceManager &M) {
  assert(R.isCharRange());
  if (!R.isValid() || M.getFileID(R.getBegin()) != M.getFileID(R.getEnd()) ||
      M.getFileID(R.getBegin()) != M.getFileID(L))
    return false;
  return L != R.getEnd() && M.isPointWithin(L, R.getBegin(), R.getEnd());
}

// Converts a half-open clang source range to an LSP range.
// Note that clang also uses closed source ranges, which this can't handle!
Range toRange(CharSourceRange R, const SourceManager &M) {
  // Clang is 1-based, LSP uses 0-based indexes.
  Position Begin;
  Begin.line = static_cast<int>(M.getSpellingLineNumber(R.getBegin())) - 1;
  Begin.character =
      static_cast<int>(M.getSpellingColumnNumber(R.getBegin())) - 1;

  Position End;
  End.line = static_cast<int>(M.getSpellingLineNumber(R.getEnd())) - 1;
  End.character = static_cast<int>(M.getSpellingColumnNumber(R.getEnd())) - 1;

  return {Begin, End};
}

// Clang diags have a location (shown as ^) and 0 or more ranges (~~~~).
// LSP needs a single range.
Range diagnosticRange(const clang::Diagnostic &D, const LangOptions &L) {
  auto &M = D.getSourceManager();
  auto Loc = M.getFileLoc(D.getLocation());
  // Accept the first range that contains the location.
  for (const auto &CR : D.getRanges()) {
    auto R = Lexer::makeFileCharRange(CR, M, L);
    if (locationInRange(Loc, R, M))
      return toRange(R, M);
  }
  // The range may be given as a fixit hint instead.
  for (const auto &F : D.getFixItHints()) {
    auto R = Lexer::makeFileCharRange(F.RemoveRange, M, L);
    if (locationInRange(Loc, R, M))
      return toRange(R, M);
  }
  // If no suitable range is found, just use the token at the location.
  auto R = Lexer::makeFileCharRange(CharSourceRange::getTokenRange(Loc), M, L);
  if (!R.isValid()) // Fall back to location only, let the editor deal with it.
    R = CharSourceRange::getCharRange(Loc);
  return toRange(R, M);
}

TextEdit toTextEdit(const FixItHint &FixIt, const SourceManager &M,
                    const LangOptions &L) {
  TextEdit Result;
  Result.range = toRange(Lexer::makeFileCharRange(FixIt.RemoveRange, M, L), M);
  Result.newText = FixIt.CodeToInsert;
  return Result;
}

llvm::Optional<DiagWithFixIts> toClangdDiag(const clang::Diagnostic &D,
                                            DiagnosticsEngine::Level Level,
                                            const LangOptions &LangOpts) {
  if (!D.hasSourceManager() || !D.getLocation().isValid() ||
      !D.getSourceManager().isInMainFile(D.getLocation())) {
    IgnoreDiagnostics::log(Level, D);
    return llvm::None;
  }

  SmallString<64> Message;
  D.FormatDiagnostic(Message);

  DiagWithFixIts Result;
  Result.Diag.range = diagnosticRange(D, LangOpts);
  Result.Diag.severity = getSeverity(Level);
  Result.Diag.message = Message.str();
  for (const FixItHint &Fix : D.getFixItHints())
    Result.FixIts.push_back(toTextEdit(Fix, D.getSourceManager(), LangOpts));
  return std::move(Result);
}

class StoreDiagsConsumer : public DiagnosticConsumer {
public:
  StoreDiagsConsumer(std::vector<DiagWithFixIts> &Output) : Output(Output) {}

  // Track language options in case we need to expand token ranges.
  void BeginSourceFile(const LangOptions &Opts, const Preprocessor *) override {
    LangOpts = Opts;
  }

  void EndSourceFile() override { LangOpts = llvm::None; }

  void HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                        const clang::Diagnostic &Info) override {
    DiagnosticConsumer::HandleDiagnostic(DiagLevel, Info);

    if (LangOpts)
      if (auto D = toClangdDiag(Info, DiagLevel, *LangOpts))
        Output.push_back(std::move(*D));
  }

private:
  std::vector<DiagWithFixIts> &Output;
  llvm::Optional<LangOptions> LangOpts;
};

} // namespace

void clangd::dumpAST(ParsedAST &AST, llvm::raw_ostream &OS) {
  AST.getASTContext().getTranslationUnitDecl()->dump(OS, true);
}

llvm::Optional<ParsedAST>
ParsedAST::Build(std::unique_ptr<clang::CompilerInvocation> CI,
                 std::shared_ptr<const PreambleData> Preamble,
                 std::unique_ptr<llvm::MemoryBuffer> Buffer,
                 std::shared_ptr<PCHContainerOperations> PCHs,
                 IntrusiveRefCntPtr<vfs::FileSystem> VFS) {
  std::vector<DiagWithFixIts> ASTDiags;
  StoreDiagsConsumer UnitDiagsConsumer(/*ref*/ ASTDiags);

  const PrecompiledPreamble *PreamblePCH =
      Preamble ? &Preamble->Preamble : nullptr;
  auto Clang = prepareCompilerInstance(
      std::move(CI), PreamblePCH, std::move(Buffer), std::move(PCHs),
      std::move(VFS), /*ref*/ UnitDiagsConsumer);
  if (!Clang)
    return llvm::None;

  // Recover resources if we crash before exiting this method.
  llvm::CrashRecoveryContextCleanupRegistrar<CompilerInstance> CICleanup(
      Clang.get());

  auto Action = llvm::make_unique<ClangdFrontendAction>();
  const FrontendInputFile &MainInput = Clang->getFrontendOpts().Inputs[0];
  if (!Action->BeginSourceFile(*Clang, MainInput)) {
    log("BeginSourceFile() failed when building AST for " +
        MainInput.getFile());
    return llvm::None;
  }
  if (!Action->Execute())
    log("Execute() failed when building AST for " + MainInput.getFile());

  // UnitDiagsConsumer is local, we can not store it in CompilerInstance that
  // has a longer lifetime.
  Clang->getDiagnostics().setClient(new IgnoreDiagnostics);

  std::vector<const Decl *> ParsedDecls = Action->takeTopLevelDecls();
  return ParsedAST(std::move(Preamble), std::move(Clang), std::move(Action),
                   std::move(ParsedDecls), std::move(ASTDiags));
}

namespace {

SourceLocation getMacroArgExpandedLocation(const SourceManager &Mgr,
                                           const FileEntry *FE, Position Pos) {
  SourceLocation InputLoc =
      Mgr.translateFileLineCol(FE, Pos.line + 1, Pos.character + 1);
  return Mgr.getMacroArgExpandedLocation(InputLoc);
}

} // namespace

void ParsedAST::ensurePreambleDeclsDeserialized() {
  if (PreambleDeclsDeserialized || !Preamble)
    return;

  std::vector<const Decl *> Resolved;
  Resolved.reserve(Preamble->TopLevelDeclIDs.size());

  ExternalASTSource &Source = *getASTContext().getExternalSource();
  for (serialization::DeclID TopLevelDecl : Preamble->TopLevelDeclIDs) {
    // Resolve the declaration ID to an actual declaration, possibly
    // deserializing the declaration in the process.
    if (Decl *D = Source.GetExternalDecl(TopLevelDecl))
      Resolved.push_back(D);
  }

  TopLevelDecls.reserve(TopLevelDecls.size() +
                        Preamble->TopLevelDeclIDs.size());
  TopLevelDecls.insert(TopLevelDecls.begin(), Resolved.begin(), Resolved.end());

  PreambleDeclsDeserialized = true;
}

ParsedAST::ParsedAST(ParsedAST &&Other) = default;

ParsedAST &ParsedAST::operator=(ParsedAST &&Other) = default;

ParsedAST::~ParsedAST() {
  if (Action) {
    Action->EndSourceFile();
  }
}

ASTContext &ParsedAST::getASTContext() { return Clang->getASTContext(); }

const ASTContext &ParsedAST::getASTContext() const {
  return Clang->getASTContext();
}

Preprocessor &ParsedAST::getPreprocessor() { return Clang->getPreprocessor(); }

std::shared_ptr<Preprocessor> ParsedAST::getPreprocessorPtr() {
  return Clang->getPreprocessorPtr();
}

const Preprocessor &ParsedAST::getPreprocessor() const {
  return Clang->getPreprocessor();
}

ArrayRef<const Decl *> ParsedAST::getTopLevelDecls() {
  ensurePreambleDeclsDeserialized();
  return TopLevelDecls;
}

const std::vector<DiagWithFixIts> &ParsedAST::getDiagnostics() const {
  return Diags;
}

std::size_t ParsedAST::getUsedBytes() const {
  auto &AST = getASTContext();
  // FIXME(ibiryukov): we do not account for the dynamically allocated part of
  // SmallVector<FixIt> inside each Diag.
  return AST.getASTAllocatedMemory() + AST.getSideTableAllocatedMemory() +
         ::getUsedBytes(TopLevelDecls) + ::getUsedBytes(Diags);
}

PreambleData::PreambleData(PrecompiledPreamble Preamble,
                           std::vector<serialization::DeclID> TopLevelDeclIDs,
                           std::vector<DiagWithFixIts> Diags)
    : Preamble(std::move(Preamble)),
      TopLevelDeclIDs(std::move(TopLevelDeclIDs)), Diags(std::move(Diags)) {}

ParsedAST::ParsedAST(std::shared_ptr<const PreambleData> Preamble,
                     std::unique_ptr<CompilerInstance> Clang,
                     std::unique_ptr<FrontendAction> Action,
                     std::vector<const Decl *> TopLevelDecls,
                     std::vector<DiagWithFixIts> Diags)
    : Preamble(std::move(Preamble)), Clang(std::move(Clang)),
      Action(std::move(Action)), Diags(std::move(Diags)),
      TopLevelDecls(std::move(TopLevelDecls)),
      PreambleDeclsDeserialized(false) {
  assert(this->Clang);
  assert(this->Action);
}

CppFile::CppFile(PathRef FileName, bool StorePreamblesInMemory,
                 std::shared_ptr<PCHContainerOperations> PCHs,
                 ASTParsedCallback ASTCallback)
    : FileName(FileName), StorePreamblesInMemory(StorePreamblesInMemory),
      PCHs(std::move(PCHs)), ASTCallback(std::move(ASTCallback)) {
  log("Created CppFile for " + FileName);
}

llvm::Optional<std::vector<DiagWithFixIts>>
CppFile::rebuild(ParseInputs &&Inputs) {
  log("Rebuilding file " + FileName + " with command [" +
      Inputs.CompileCommand.Directory + "] " +
      llvm::join(Inputs.CompileCommand.CommandLine, " "));

  std::vector<const char *> ArgStrs;
  for (const auto &S : Inputs.CompileCommand.CommandLine)
    ArgStrs.push_back(S.c_str());

  if (Inputs.FS->setCurrentWorkingDirectory(Inputs.CompileCommand.Directory)) {
    log("Couldn't set working directory");
    // We run parsing anyway, our lit-tests rely on results for non-existing
    // working dirs.
  }

  // Prepare CompilerInvocation.
  std::unique_ptr<CompilerInvocation> CI;
  {
    // FIXME(ibiryukov): store diagnostics from CommandLine when we start
    // reporting them.
    IgnoreDiagnostics IgnoreDiagnostics;
    IntrusiveRefCntPtr<DiagnosticsEngine> CommandLineDiagsEngine =
        CompilerInstance::createDiagnostics(new DiagnosticOptions,
                                            &IgnoreDiagnostics, false);
    CI = createInvocationFromCommandLine(ArgStrs, CommandLineDiagsEngine,
                                         Inputs.FS);
    if (!CI) {
      log("Could not build CompilerInvocation for file " + FileName);
      AST = llvm::None;
      Preamble = nullptr;
      return llvm::None;
    }
    // createInvocationFromCommandLine sets DisableFree.
    CI->getFrontendOpts().DisableFree = false;
  }

  std::unique_ptr<llvm::MemoryBuffer> ContentsBuffer =
      llvm::MemoryBuffer::getMemBufferCopy(Inputs.Contents, FileName);

  // Compute updated Preamble.
  std::shared_ptr<const PreambleData> NewPreamble =
      rebuildPreamble(*CI, Inputs.FS, *ContentsBuffer);

  // Remove current AST to avoid wasting memory.
  AST = llvm::None;
  // Compute updated AST.
  llvm::Optional<ParsedAST> NewAST;
  {
    trace::Span Tracer("Build");
    SPAN_ATTACH(Tracer, "File", FileName);
    NewAST = ParsedAST::Build(std::move(CI), NewPreamble,
                              std::move(ContentsBuffer), PCHs, Inputs.FS);
  }

  std::vector<DiagWithFixIts> Diagnostics;
  if (NewAST) {
    // Collect diagnostics from both the preamble and the AST.
    if (NewPreamble)
      Diagnostics.insert(Diagnostics.begin(), NewPreamble->Diags.begin(),
                         NewPreamble->Diags.end());
    Diagnostics.insert(Diagnostics.end(), NewAST->getDiagnostics().begin(),
                       NewAST->getDiagnostics().end());
  }
  if (ASTCallback && NewAST) {
    trace::Span Tracer("Running ASTCallback");
    ASTCallback(FileName, NewAST.getPointer());
  }

  // Write the results of rebuild into class fields.
  Preamble = std::move(NewPreamble);
  AST = std::move(NewAST);
  return Diagnostics;
}

const std::shared_ptr<const PreambleData> &CppFile::getPreamble() const {
  return Preamble;
}

ParsedAST *CppFile::getAST() const {
  // We could add mutable to AST instead of const_cast here, but that would also
  // allow writing to AST from const methods.
  return AST ? const_cast<ParsedAST *>(AST.getPointer()) : nullptr;
}

std::size_t CppFile::getUsedBytes() const {
  std::size_t Total = 0;
  if (AST)
    Total += AST->getUsedBytes();
  if (StorePreamblesInMemory && Preamble)
    Total += Preamble->Preamble.getSize();
  return Total;
}

std::shared_ptr<const PreambleData>
CppFile::rebuildPreamble(CompilerInvocation &CI,
                         IntrusiveRefCntPtr<vfs::FileSystem> FS,
                         llvm::MemoryBuffer &ContentsBuffer) const {
  const auto &OldPreamble = this->Preamble;
  auto Bounds = ComputePreambleBounds(*CI.getLangOpts(), &ContentsBuffer, 0);
  if (OldPreamble &&
      OldPreamble->Preamble.CanReuse(CI, &ContentsBuffer, Bounds, FS.get())) {
    log("Reusing preamble for file " + Twine(FileName));
    return OldPreamble;
  }
  log("Preamble for file " + Twine(FileName) +
      " cannot be reused. Attempting to rebuild it.");

  trace::Span Tracer("Preamble");
  SPAN_ATTACH(Tracer, "File", FileName);
  std::vector<DiagWithFixIts> PreambleDiags;
  StoreDiagsConsumer PreambleDiagnosticsConsumer(/*ref*/ PreambleDiags);
  IntrusiveRefCntPtr<DiagnosticsEngine> PreambleDiagsEngine =
      CompilerInstance::createDiagnostics(&CI.getDiagnosticOpts(),
                                          &PreambleDiagnosticsConsumer, false);

  // Skip function bodies when building the preamble to speed up building
  // the preamble and make it smaller.
  assert(!CI.getFrontendOpts().SkipFunctionBodies);
  CI.getFrontendOpts().SkipFunctionBodies = true;

  CppFilePreambleCallbacks SerializedDeclsCollector;
  auto BuiltPreamble = PrecompiledPreamble::Build(
      CI, &ContentsBuffer, Bounds, *PreambleDiagsEngine, FS, PCHs,
      /*StoreInMemory=*/StorePreamblesInMemory, SerializedDeclsCollector);

  // When building the AST for the main file, we do want the function
  // bodies.
  CI.getFrontendOpts().SkipFunctionBodies = false;

  if (BuiltPreamble) {
    log("Built preamble of size " + Twine(BuiltPreamble->getSize()) +
        " for file " + Twine(FileName));

    return std::make_shared<PreambleData>(
        std::move(*BuiltPreamble),
        SerializedDeclsCollector.takeTopLevelDeclIDs(),
        std::move(PreambleDiags));
  } else {
    log("Could not build a preamble for file " + Twine(FileName));
    return nullptr;
  }
}

SourceLocation clangd::getBeginningOfIdentifier(ParsedAST &Unit,
                                                const Position &Pos,
                                                const FileEntry *FE) {
  // The language server protocol uses zero-based line and column numbers.
  // Clang uses one-based numbers.

  const ASTContext &AST = Unit.getASTContext();
  const SourceManager &SourceMgr = AST.getSourceManager();

  SourceLocation InputLocation =
      getMacroArgExpandedLocation(SourceMgr, FE, Pos);
  if (Pos.character == 0) {
    return InputLocation;
  }

  // This handle cases where the position is in the middle of a token or right
  // after the end of a token. In theory we could just use GetBeginningOfToken
  // to find the start of the token at the input position, but this doesn't
  // work when right after the end, i.e. foo|.
  // So try to go back by one and see if we're still inside an identifier
  // token. If so, Take the beginning of this token.
  // (It should be the same identifier because you can't have two adjacent
  // identifiers without another token in between.)
  Position PosCharBehind = Pos;
  --PosCharBehind.character;

  SourceLocation PeekBeforeLocation =
      getMacroArgExpandedLocation(SourceMgr, FE, PosCharBehind);
  Token Result;
  if (Lexer::getRawToken(PeekBeforeLocation, Result, SourceMgr,
                         AST.getLangOpts(), false)) {
    // getRawToken failed, just use InputLocation.
    return InputLocation;
  }

  if (Result.is(tok::raw_identifier)) {
    return Lexer::GetBeginningOfToken(PeekBeforeLocation, SourceMgr,
                                      AST.getLangOpts());
  }

  return InputLocation;
}
