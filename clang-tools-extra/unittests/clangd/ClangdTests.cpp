//===-- ClangdTests.cpp - Clangd unit tests ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ClangdServer.h"
#include "clang/Basic/VirtualFileSystem.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Config/config.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace clang {
namespace vfs {

/// An implementation of vfs::FileSystem that only allows access to
/// files and folders inside a set of whitelisted directories.
///
/// FIXME(ibiryukov): should it also emulate access to parents of whitelisted
/// directories with only whitelisted contents?
class FilteredFileSystem : public vfs::FileSystem {
public:
  /// The paths inside \p WhitelistedDirs should be absolute
  FilteredFileSystem(std::vector<std::string> WhitelistedDirs,
                     IntrusiveRefCntPtr<vfs::FileSystem> InnerFS)
      : WhitelistedDirs(std::move(WhitelistedDirs)), InnerFS(InnerFS) {
    assert(std::all_of(WhitelistedDirs.begin(), WhitelistedDirs.end(),
                       [](const std::string &Path) -> bool {
                         return llvm::sys::path::is_absolute(Path);
                       }) &&
           "Not all WhitelistedDirs are absolute");
  }

  virtual llvm::ErrorOr<Status> status(const Twine &Path) {
    if (!isInsideWhitelistedDir(Path))
      return llvm::errc::no_such_file_or_directory;
    return InnerFS->status(Path);
  }

  virtual llvm::ErrorOr<std::unique_ptr<File>>
  openFileForRead(const Twine &Path) {
    if (!isInsideWhitelistedDir(Path))
      return llvm::errc::no_such_file_or_directory;
    return InnerFS->openFileForRead(Path);
  }

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
  getBufferForFile(const Twine &Name, int64_t FileSize = -1,
                   bool RequiresNullTerminator = true,
                   bool IsVolatile = false) {
    if (!isInsideWhitelistedDir(Name))
      return llvm::errc::no_such_file_or_directory;
    return InnerFS->getBufferForFile(Name, FileSize, RequiresNullTerminator,
                                     IsVolatile);
  }

  virtual directory_iterator dir_begin(const Twine &Dir, std::error_code &EC) {
    if (!isInsideWhitelistedDir(Dir)) {
      EC = llvm::errc::no_such_file_or_directory;
      return directory_iterator();
    }
    return InnerFS->dir_begin(Dir, EC);
  }

  virtual std::error_code setCurrentWorkingDirectory(const Twine &Path) {
    return InnerFS->setCurrentWorkingDirectory(Path);
  }

  virtual llvm::ErrorOr<std::string> getCurrentWorkingDirectory() const {
    return InnerFS->getCurrentWorkingDirectory();
  }

  bool exists(const Twine &Path) {
    if (!isInsideWhitelistedDir(Path))
      return false;
    return InnerFS->exists(Path);
  }

  std::error_code makeAbsolute(SmallVectorImpl<char> &Path) const {
    return InnerFS->makeAbsolute(Path);
  }

private:
  bool isInsideWhitelistedDir(const Twine &InputPath) const {
    SmallString<128> Path;
    InputPath.toVector(Path);

    if (makeAbsolute(Path))
      return false;

    for (const auto &Dir : WhitelistedDirs) {
      if (Path.startswith(Dir))
        return true;
    }
    return false;
  }

  std::vector<std::string> WhitelistedDirs;
  IntrusiveRefCntPtr<vfs::FileSystem> InnerFS;
};

/// Create a vfs::FileSystem that has access only to temporary directories
/// (obtained by calling system_temp_directory).
IntrusiveRefCntPtr<vfs::FileSystem> getTempOnlyFS() {
  llvm::SmallString<128> TmpDir1;
  llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/false, TmpDir1);
  llvm::SmallString<128> TmpDir2;
  llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/true, TmpDir2);

  std::vector<std::string> TmpDirs;
  TmpDirs.push_back(TmpDir1.str());
  if (TmpDir1 != TmpDir2)
    TmpDirs.push_back(TmpDir2.str());
  return new vfs::FilteredFileSystem(std::move(TmpDirs),
                                     vfs::getRealFileSystem());
}
} // namespace vfs

namespace clangd {
namespace {

class ErrorCheckingDiagConsumer : public DiagnosticsConsumer {
public:
  void onDiagnosticsReady(PathRef File,
                          std::vector<DiagWithFixIts> Diagnostics) override {
    bool HadError = false;
    for (const auto &DiagAndFixIts : Diagnostics) {
      // FIXME: severities returned by clangd should have a descriptive
      // diagnostic severity enum
      const int ErrorSeverity = 1;
      HadError = DiagAndFixIts.Diag.severity == ErrorSeverity;
    }

    std::lock_guard<std::mutex> Lock(Mutex);
    HadErrorInLastDiags = HadError;
  }

  bool hadErrorInLastDiags() {
    std::lock_guard<std::mutex> Lock(Mutex);
    return HadErrorInLastDiags;
  }

private:
  std::mutex Mutex;
  bool HadErrorInLastDiags = false;
};

class MockCompilationDatabase : public GlobalCompilationDatabase {
public:
  std::vector<tooling::CompileCommand>
  getCompileCommands(PathRef File) override {
    return {};
  }
};

class MockFSProvider : public FileSystemProvider {
public:
  IntrusiveRefCntPtr<vfs::FileSystem> getFileSystem() override {
    IntrusiveRefCntPtr<vfs::InMemoryFileSystem> MemFS(
        new vfs::InMemoryFileSystem);
    for (auto &FileAndContents : Files)
      MemFS->addFile(FileAndContents.first(), time_t(),
                     llvm::MemoryBuffer::getMemBuffer(FileAndContents.second,
                                                      FileAndContents.first()));

    auto OverlayFS = IntrusiveRefCntPtr<vfs::OverlayFileSystem>(
        new vfs::OverlayFileSystem(vfs::getTempOnlyFS()));
    OverlayFS->pushOverlay(std::move(MemFS));
    return OverlayFS;
  }

  llvm::StringMap<std::string> Files;
};

/// Replaces all patterns of the form 0x123abc with spaces
std::string replacePtrsInDump(std::string const &Dump) {
  llvm::Regex RE("0x[0-9a-fA-F]+");
  llvm::SmallVector<StringRef, 1> Matches;
  llvm::StringRef Pending = Dump;

  std::string Result;
  while (RE.match(Pending, &Matches)) {
    assert(Matches.size() == 1 && "Exactly one match expected");
    auto MatchPos = Matches[0].data() - Pending.data();

    Result += Pending.take_front(MatchPos);
    Pending = Pending.drop_front(MatchPos + Matches[0].size());
  }
  Result += Pending;

  return Result;
}

std::string dumpASTWithoutMemoryLocs(ClangdServer &Server, PathRef File) {
  auto DumpWithMemLocs = Server.dumpAST(File);
  return replacePtrsInDump(DumpWithMemLocs);
}

template <class T>
std::unique_ptr<T> getAndMove(std::unique_ptr<T> Ptr, T *&Output) {
  Output = Ptr.get();
  return Ptr;
}
} // namespace

class ClangdVFSTest : public ::testing::Test {
protected:
  SmallString<16> getVirtualTestRoot() {
#ifdef LLVM_ON_WIN32
    return SmallString<16>("C:\\clangd-test");
#else
    return SmallString<16>("/clangd-test");
#endif
  }

  llvm::SmallString<32> getVirtualTestFilePath(PathRef File) {
    assert(llvm::sys::path::is_relative(File) && "FileName should be relative");

    llvm::SmallString<32> Path;
    llvm::sys::path::append(Path, getVirtualTestRoot(), File);
    return Path;
  }

  std::string parseSourceAndDumpAST(
      PathRef SourceFileRelPath, StringRef SourceContents,
      std::vector<std::pair<PathRef, StringRef>> ExtraFiles = {},
      bool ExpectErrors = false) {
    MockFSProvider *FS;
    ErrorCheckingDiagConsumer *DiagConsumer;
    ClangdServer Server(
        llvm::make_unique<MockCompilationDatabase>(),
        getAndMove(llvm::make_unique<ErrorCheckingDiagConsumer>(),
                   DiagConsumer),
        getAndMove(llvm::make_unique<MockFSProvider>(), FS),
        /*RunSynchronously=*/false);
    for (const auto &FileWithContents : ExtraFiles)
      FS->Files[getVirtualTestFilePath(FileWithContents.first)] =
          FileWithContents.second;

    auto SourceFilename = getVirtualTestFilePath(SourceFileRelPath);
    Server.addDocument(SourceFilename, SourceContents);
    auto Result = dumpASTWithoutMemoryLocs(Server, SourceFilename);
    EXPECT_EQ(ExpectErrors, DiagConsumer->hadErrorInLastDiags());
    return Result;
  }
};

TEST_F(ClangdVFSTest, Parse) {
  // FIXME: figure out a stable format for AST dumps, so that we can check the
  // output of the dump itself is equal to the expected one, not just that it's
  // different.
  auto Empty = parseSourceAndDumpAST("foo.cpp", "", {});
  auto OneDecl = parseSourceAndDumpAST("foo.cpp", "int a;", {});
  auto SomeDecls = parseSourceAndDumpAST("foo.cpp", "int a; int b; int c;", {});
  EXPECT_NE(Empty, OneDecl);
  EXPECT_NE(Empty, SomeDecls);
  EXPECT_NE(SomeDecls, OneDecl);

  auto Empty2 = parseSourceAndDumpAST("foo.cpp", "");
  auto OneDecl2 = parseSourceAndDumpAST("foo.cpp", "int a;");
  auto SomeDecls2 = parseSourceAndDumpAST("foo.cpp", "int a; int b; int c;");
  EXPECT_EQ(Empty, Empty2);
  EXPECT_EQ(OneDecl, OneDecl2);
  EXPECT_EQ(SomeDecls, SomeDecls2);
}

TEST_F(ClangdVFSTest, ParseWithHeader) {
  parseSourceAndDumpAST("foo.cpp", "#include \"foo.h\"", {},
                        /*ExpectErrors=*/true);
  parseSourceAndDumpAST("foo.cpp", "#include \"foo.h\"", {{"foo.h", ""}},
                        /*ExpectErrors=*/false);

  const auto SourceContents = R"cpp(
#include "foo.h"
int b = a;
)cpp";
  parseSourceAndDumpAST("foo.cpp", SourceContents, {{"foo.h", ""}},
                        /*ExpectErrors=*/true);
  parseSourceAndDumpAST("foo.cpp", SourceContents, {{"foo.h", "int a;"}},
                        /*ExpectErrors=*/false);
}

TEST_F(ClangdVFSTest, Reparse) {
  MockFSProvider *FS;
  ErrorCheckingDiagConsumer *DiagConsumer;
  ClangdServer Server(
      llvm::make_unique<MockCompilationDatabase>(),
      getAndMove(llvm::make_unique<ErrorCheckingDiagConsumer>(), DiagConsumer),
      getAndMove(llvm::make_unique<MockFSProvider>(), FS),
      /*RunSynchronously=*/false);

  const auto SourceContents = R"cpp(
#include "foo.h"
int b = a;
)cpp";

  auto FooCpp = getVirtualTestFilePath("foo.cpp");
  auto FooH = getVirtualTestFilePath("foo.h");

  FS->Files[FooH] = "int a;";
  FS->Files[FooCpp] = SourceContents;

  Server.addDocument(FooCpp, SourceContents);
  auto DumpParse1 = dumpASTWithoutMemoryLocs(Server, FooCpp);
  EXPECT_FALSE(DiagConsumer->hadErrorInLastDiags());

  Server.addDocument(FooCpp, "");
  auto DumpParseEmpty = dumpASTWithoutMemoryLocs(Server, FooCpp);
  EXPECT_FALSE(DiagConsumer->hadErrorInLastDiags());

  Server.addDocument(FooCpp, SourceContents);
  auto DumpParse2 = dumpASTWithoutMemoryLocs(Server, FooCpp);
  EXPECT_FALSE(DiagConsumer->hadErrorInLastDiags());

  EXPECT_EQ(DumpParse1, DumpParse2);
  EXPECT_NE(DumpParse1, DumpParseEmpty);
}

TEST_F(ClangdVFSTest, ReparseOnHeaderChange) {
  MockFSProvider *FS;
  ErrorCheckingDiagConsumer *DiagConsumer;

  ClangdServer Server(
      llvm::make_unique<MockCompilationDatabase>(),
      getAndMove(llvm::make_unique<ErrorCheckingDiagConsumer>(), DiagConsumer),
      getAndMove(llvm::make_unique<MockFSProvider>(), FS),
      /*RunSynchronously=*/false);

  const auto SourceContents = R"cpp(
#include "foo.h"
int b = a;
)cpp";

  auto FooCpp = getVirtualTestFilePath("foo.cpp");
  auto FooH = getVirtualTestFilePath("foo.h");

  FS->Files[FooH] = "int a;";
  FS->Files[FooCpp] = SourceContents;

  Server.addDocument(FooCpp, SourceContents);
  auto DumpParse1 = dumpASTWithoutMemoryLocs(Server, FooCpp);
  EXPECT_FALSE(DiagConsumer->hadErrorInLastDiags());

  FS->Files[FooH] = "";
  Server.forceReparse(FooCpp);
  auto DumpParseDifferent = dumpASTWithoutMemoryLocs(Server, FooCpp);
  EXPECT_TRUE(DiagConsumer->hadErrorInLastDiags());

  FS->Files[FooH] = "int a;";
  Server.forceReparse(FooCpp);
  auto DumpParse2 = dumpASTWithoutMemoryLocs(Server, FooCpp);
  EXPECT_FALSE(DiagConsumer->hadErrorInLastDiags());

  EXPECT_EQ(DumpParse1, DumpParse2);
  EXPECT_NE(DumpParse1, DumpParseDifferent);
}

} // namespace clangd
} // namespace clang
