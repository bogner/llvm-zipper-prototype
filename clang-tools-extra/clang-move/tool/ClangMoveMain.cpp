//===-- ClangMoveMain.cpp - move defintion to new file ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ClangMove.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/Path.h"
#include <set>
#include <string>

using namespace clang;
using namespace llvm;

namespace {

std::error_code CreateNewFile(const llvm::Twine &path) {
  int fd = 0;
  if (std::error_code ec =
          llvm::sys::fs::openFileForWrite(path, fd, llvm::sys::fs::F_Text))
    return ec;

  return llvm::sys::Process::SafelyCloseFileDescriptor(fd);
}

cl::OptionCategory ClangMoveCategory("clang-move options");

cl::opt<std::string> Name("name", cl::desc("The name of class being moved."),
                          cl::cat(ClangMoveCategory));

cl::opt<std::string>
    OldHeader("old_header",
              cl::desc("The relative/absolute file path of old header."),
              cl::cat(ClangMoveCategory));

cl::opt<std::string>
    OldCC("old_cc", cl::desc("The relative/absolute file path of old cc."),
          cl::cat(ClangMoveCategory));

cl::opt<std::string>
    NewHeader("new_header",
              cl::desc("The relative/absolute file path of new header."),
              cl::cat(ClangMoveCategory));

cl::opt<std::string>
    NewCC("new_cc", cl::desc("The relative/absolute file path of new cc."),
          cl::cat(ClangMoveCategory));

cl::opt<std::string>
    Style("style",
          cl::desc("The style name used for reformatting. Default is \"llvm\""),
          cl::init("llvm"), cl::cat(ClangMoveCategory));

cl::opt<bool> Dump("dump_result",
                   cl::desc("Dump results in JSON format to stdout."),
                   cl::cat(ClangMoveCategory));

} // namespace

int main(int argc, const char **argv) {
  tooling::CommonOptionsParser OptionsParser(argc, argv, ClangMoveCategory);
  tooling::RefactoringTool Tool(OptionsParser.getCompilations(),
                                OptionsParser.getSourcePathList());
  move::ClangMoveTool::MoveDefinitionSpec Spec;
  Spec.Name = Name;
  Spec.OldHeader = OldHeader;
  Spec.NewHeader = NewHeader;
  Spec.OldCC = OldCC;
  Spec.NewCC = NewCC;

  llvm::SmallString<128> InitialDirectory;
  if (std::error_code EC = llvm::sys::fs::current_path(InitialDirectory))
    llvm::report_fatal_error("Cannot detect current path: " +
                             Twine(EC.message()));

  auto Factory = llvm::make_unique<clang::move::ClangMoveActionFactory>(
      Spec, Tool.getReplacements(), InitialDirectory.str(), Style);

  int CodeStatus = Tool.run(Factory.get());
  if (CodeStatus)
    return CodeStatus;

  if (!NewCC.empty())
    CreateNewFile(NewCC);
  if (!NewHeader.empty())
    CreateNewFile(NewHeader);

  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts(new DiagnosticOptions());
  clang::TextDiagnosticPrinter DiagnosticPrinter(errs(), &*DiagOpts);
  DiagnosticsEngine Diagnostics(
      IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs()), &*DiagOpts,
      &DiagnosticPrinter, false);
  auto &FileMgr = Tool.getFiles();
  SourceManager SM(Diagnostics, FileMgr);
  Rewriter Rewrite(SM, LangOptions());

  if (!formatAndApplyAllReplacements(Tool.getReplacements(), Rewrite, Style)) {
    llvm::errs() << "Failed applying all replacements.\n";
    return 1;
  }

  if (Dump) {
    std::set<llvm::StringRef> Files;
    for (const auto &it : Tool.getReplacements())
      Files.insert(it.first);
    auto WriteToJson = [&](llvm::raw_ostream &OS) {
      OS << "[\n";
      for (auto File : Files) {
        OS << "  {\n";
        OS << "    \"FilePath\": \"" << File << "\",\n";
        const auto *Entry = FileMgr.getFile(File);
        auto ID = SM.translateFile(Entry);
        std::string Content;
        llvm::raw_string_ostream ContentStream(Content);
        Rewrite.getEditBuffer(ID).write(ContentStream);
        OS << "    \"SourceText\": \""
           << llvm::yaml::escape(ContentStream.str()) << "\"\n";
        OS << "  }";
        if (File != *(--Files.end()))
          OS << ",\n";
      }
      OS << "\n]\n";
    };
    WriteToJson(llvm::outs());
    return 0;
  }

  return Rewrite.overwriteChangedFiles();
}
