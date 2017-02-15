//===--- ASTManager.h - Clang AST manager -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_ASTMANAGER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_ASTMANAGER_H

#include "DocumentStore.h"
#include "JSONRPCDispatcher.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include <condition_variable>
#include <deque>
#include <thread>

namespace clang {
class ASTUnit;
class DiagnosticsEngine;
class PCHContainerOperations;
namespace tooling {
class CompilationDatabase;
} // namespace tooling

namespace clangd {

class ASTManager : public DocumentStoreListener {
public:
  ASTManager(JSONOutput &Output, DocumentStore &Store);
  ~ASTManager() override;

  void onDocumentAdd(StringRef Uri) override;
  // FIXME: Implement onDocumentRemove
  // FIXME: Implement codeComplete

private:
  JSONOutput &Output;
  DocumentStore &Store;

  /// Loads a compilation database for URI. May return nullptr if it fails. The
  /// database is cached for subsequent accesses.
  clang::tooling::CompilationDatabase *
  getOrCreateCompilationDatabaseForFile(StringRef Uri);
  // Craetes a new ASTUnit for the document at Uri.
  // FIXME: This calls chdir internally, which is thread unsafe.
  std::unique_ptr<clang::ASTUnit>
  createASTUnitForFile(StringRef Uri, const DocumentStore &Docs);

  void runWorker();

  /// Clang objects.
  llvm::StringMap<std::unique_ptr<clang::ASTUnit>> ASTs;
  llvm::StringMap<std::unique_ptr<clang::tooling::CompilationDatabase>>
      CompilationDatabases;
  std::shared_ptr<clang::PCHContainerOperations> PCHs;

  /// We run parsing on a separate thread. This thread looks into PendingRequest
  /// as a 'one element work queue' as long as RequestIsPending is true.
  std::thread ClangWorker;
  /// Queue of requests.
  std::deque<std::string> RequestQueue;
  /// Setting Done to true will make the worker thread terminate.
  bool Done = false;
  /// Condition variable to wake up the worker thread.
  std::condition_variable ClangRequestCV;
  /// Lock for accesses to RequestQueue and Done.
  std::mutex RequestLock;
};

} // namespace clangd
} // namespace clang

#endif
