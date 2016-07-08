//===-- IncludeFixerTest.cpp - Include fixer unit tests -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "InMemorySymbolIndex.h"
#include "IncludeFixer.h"
#include "SymbolIndexManager.h"
#include "unittests/Tooling/RewriterTestContext.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"

namespace clang {
namespace include_fixer {
namespace {

using find_all_symbols::SymbolInfo;

static bool runOnCode(tooling::ToolAction *ToolAction, StringRef Code,
                      StringRef FileName,
                      const std::vector<std::string> &ExtraArgs) {
  llvm::IntrusiveRefCntPtr<vfs::InMemoryFileSystem> InMemoryFileSystem(
      new vfs::InMemoryFileSystem);
  llvm::IntrusiveRefCntPtr<FileManager> Files(
      new FileManager(FileSystemOptions(), InMemoryFileSystem));
  // FIXME: Investigate why -fms-compatibility breaks tests.
  std::vector<std::string> Args = {"include_fixer", "-fsyntax-only",
                                   "-fno-ms-compatibility", FileName};
  Args.insert(Args.end(), ExtraArgs.begin(), ExtraArgs.end());
  tooling::ToolInvocation Invocation(
      Args, ToolAction, Files.get(),
      std::make_shared<PCHContainerOperations>());

  InMemoryFileSystem->addFile(FileName, 0,
                              llvm::MemoryBuffer::getMemBuffer(Code));

  InMemoryFileSystem->addFile("foo.h", 0,
                              llvm::MemoryBuffer::getMemBuffer("\n"));
  InMemoryFileSystem->addFile("dir/bar.h", 0,
                              llvm::MemoryBuffer::getMemBuffer("\n"));
  InMemoryFileSystem->addFile("dir/otherdir/qux.h", 0,
                              llvm::MemoryBuffer::getMemBuffer("\n"));
  InMemoryFileSystem->addFile("header.h", 0,
                              llvm::MemoryBuffer::getMemBuffer("bar b;"));
  return Invocation.run();
}

static std::string runIncludeFixer(
    StringRef Code,
    const std::vector<std::string> &ExtraArgs = std::vector<std::string>()) {
  std::vector<SymbolInfo> Symbols = {
      SymbolInfo("string", SymbolInfo::SymbolKind::Class, "<string>", 1,
                 {{SymbolInfo::ContextType::Namespace, "std"}}),
      SymbolInfo("sting", SymbolInfo::SymbolKind::Class, "\"sting\"", 1,
                 {{SymbolInfo::ContextType::Namespace, "std"}}),
      SymbolInfo("foo", SymbolInfo::SymbolKind::Class, "\"dir/otherdir/qux.h\"",
                 1, {{SymbolInfo::ContextType::Namespace, "b"},
                     {SymbolInfo::ContextType::Namespace, "a"}}),
      SymbolInfo("bar", SymbolInfo::SymbolKind::Class, "\"bar.h\"", 1,
                 {{SymbolInfo::ContextType::Namespace, "b"},
                  {SymbolInfo::ContextType::Namespace, "a"}}),
      SymbolInfo("Green", SymbolInfo::SymbolKind::Class, "\"color.h\"", 1,
                 {{SymbolInfo::ContextType::EnumDecl, "Color"},
                  {SymbolInfo::ContextType::Namespace, "b"},
                  {SymbolInfo::ContextType::Namespace, "a"}}),
      SymbolInfo("Vector", SymbolInfo::SymbolKind::Class, "\"Vector.h\"", 1,
                 {{SymbolInfo::ContextType::Namespace, "__a"},
                  {SymbolInfo::ContextType::Namespace, "a"}},
                 /*num_occurrences=*/2),
      SymbolInfo("Vector", SymbolInfo::SymbolKind::Class, "\"Vector.h\"", 2,
                 {{SymbolInfo::ContextType::Namespace, "a"}},
                 /*num_occurrences=*/1),
  };
  auto SymbolIndexMgr = llvm::make_unique<include_fixer::SymbolIndexManager>();
  SymbolIndexMgr->addSymbolIndex(
      llvm::make_unique<include_fixer::InMemorySymbolIndex>(Symbols));

  IncludeFixerContext FixerContext;
  IncludeFixerActionFactory Factory(*SymbolIndexMgr, FixerContext, "llvm");

  std::string FakeFileName = "input.cc";
  runOnCode(&Factory, Code, FakeFileName, ExtraArgs);
  if (FixerContext.getMatchedSymbols().empty())
    return Code;
  tooling::Replacements Replacements =
      clang::include_fixer::createInsertHeaderReplacements(
          Code, FakeFileName, FixerContext.getHeaders().front());
  clang::RewriterTestContext Context;
  clang::FileID ID = Context.createInMemoryFile(FakeFileName, Code);
  if (FixerContext.getSymbolRange().getLength() > 0)
    Replacements.insert(FixerContext.createSymbolReplacement(FakeFileName, 0));
  clang::tooling::applyAllReplacements(Replacements, Context.Rewrite);
  return Context.getRewrittenText(ID);
}

TEST(IncludeFixer, Typo) {
  EXPECT_EQ("#include <string>\nstd::string foo;\n",
            runIncludeFixer("std::string foo;\n"));

  // FIXME: the current version of include-fixer does not get this test case
  // right - header should be inserted before definition.
  EXPECT_EQ(
      "// comment\n#include \"foo.h\"\nstd::string foo;\n"
      "#include \"dir/bar.h\"\n#include <string>\n",
      runIncludeFixer("// comment\n#include \"foo.h\"\nstd::string foo;\n"
                      "#include \"dir/bar.h\"\n"));

  EXPECT_EQ("#include \"foo.h\"\n#include <string>\nstd::string foo;\n",
            runIncludeFixer("#include \"foo.h\"\nstd::string foo;\n"));

  EXPECT_EQ(
      "#include \"foo.h\"\n#include <string>\nstd::string::size_type foo;\n",
      runIncludeFixer("#include \"foo.h\"\nstd::string::size_type foo;\n"));

  EXPECT_EQ("#include <string>\nstd::string foo;\n",
            runIncludeFixer("string foo;\n"));

  // Should not match std::string.
  EXPECT_EQ("::string foo;\n", runIncludeFixer("::string foo;\n"));
}

TEST(IncludeFixer, IncompleteType) {
  EXPECT_EQ(
      "#include \"foo.h\"\n#include <string>\n"
      "namespace std {\nclass string;\n}\nstd::string foo;\n",
      runIncludeFixer("#include \"foo.h\"\n"
                      "namespace std {\nclass string;\n}\nstring foo;\n"));
}

TEST(IncludeFixer, MinimizeInclude) {
  std::vector<std::string> IncludePath = {"-Idir/"};
  EXPECT_EQ("#include \"otherdir/qux.h\"\na::b::foo bar;\n",
            runIncludeFixer("a::b::foo bar;\n", IncludePath));

  IncludePath = {"-isystemdir"};
  EXPECT_EQ("#include <otherdir/qux.h>\na::b::foo bar;\n",
            runIncludeFixer("a::b::foo bar;\n", IncludePath));

  IncludePath = {"-iquotedir"};
  EXPECT_EQ("#include \"otherdir/qux.h\"\na::b::foo bar;\n",
            runIncludeFixer("a::b::foo bar;\n", IncludePath));

  IncludePath = {"-Idir", "-Idir/otherdir"};
  EXPECT_EQ("#include \"qux.h\"\na::b::foo bar;\n",
            runIncludeFixer("a::b::foo bar;\n", IncludePath));
}

TEST(IncludeFixer, NestedName) {
  EXPECT_EQ("#include \"dir/otherdir/qux.h\"\n"
            "int x = a::b::foo(0);\n",
            runIncludeFixer("int x = a::b::foo(0);\n"));

  // FIXME: Handle simple macros.
  EXPECT_EQ("#define FOO a::b::foo\nint x = FOO;\n",
            runIncludeFixer("#define FOO a::b::foo\nint x = FOO;\n"));
  EXPECT_EQ("#define FOO(x) a::##x\nint x = FOO(b::foo);\n",
            runIncludeFixer("#define FOO(x) a::##x\nint x = FOO(b::foo);\n"));

  // The empty namespace is cleaned up by clang-format after include-fixer
  // finishes.
  EXPECT_EQ("#include \"dir/otherdir/qux.h\"\n"
            "\nint a = a::b::foo(0);\n",
            runIncludeFixer("namespace a {}\nint a = a::b::foo(0);\n"));
}

TEST(IncludeFixer, MultipleMissingSymbols) {
  EXPECT_EQ("#include <string>\nstd::string bar;\nstd::sting foo;\n",
            runIncludeFixer("std::string bar;\nstd::sting foo;\n"));
}

TEST(IncludeFixer, ScopedNamespaceSymbols) {
  EXPECT_EQ("#include \"bar.h\"\nnamespace a {\nb::bar b;\n}",
            runIncludeFixer("namespace a {\nb::bar b;\n}"));
  EXPECT_EQ("#include \"bar.h\"\nnamespace A {\na::b::bar b;\n}",
            runIncludeFixer("namespace A {\na::b::bar b;\n}"));
  EXPECT_EQ("#include \"bar.h\"\nnamespace a {\nvoid func() { b::bar b; }\n}",
            runIncludeFixer("namespace a {\nvoid func() { b::bar b; }\n}"));
  EXPECT_EQ("namespace A { c::b::bar b; }\n",
            runIncludeFixer("namespace A { c::b::bar b; }\n"));
  // FIXME: The header should not be added here. Remove this after we support
  // full match.
  EXPECT_EQ("#include \"bar.h\"\nnamespace A {\na::b::bar b;\n}",
            runIncludeFixer("namespace A {\nb::bar b;\n}"));
}

TEST(IncludeFixer, EnumConstantSymbols) {
  EXPECT_EQ("#include \"color.h\"\nint test = a::b::Green;\n",
            runIncludeFixer("int test = a::b::Green;\n"));
}

TEST(IncludeFixer, IgnoreSymbolFromHeader) {
  std::string Code = "#include \"header.h\"";
  EXPECT_EQ(Code, runIncludeFixer(Code));
}

// FIXME: add test cases for inserting and sorting multiple headers when
// include-fixer supports multiple headers insertion.
TEST(IncludeFixer, InsertAndSortSingleHeader) {
  // Insert one header.
  std::string Code = "#include \"a.h\"\n"
                     "#include \"foo.h\"\n"
                     "\n"
                     "namespace a { b::bar b; }";
  std::string Expected = "#include \"a.h\"\n"
                         "#include \"bar.h\"\n"
                         "#include \"foo.h\"\n"
                         "\n"
                         "namespace a { b::bar b; }";
  EXPECT_EQ(Expected, runIncludeFixer(Code));
}

TEST(IncludeFixer, DoNotDeleteMatchedSymbol) {
  EXPECT_EQ("#include \"Vector.h\"\na::Vector v;",
            runIncludeFixer("a::Vector v;"));
}

TEST(IncludeFixer, FixNamespaceQualifiers) {
  EXPECT_EQ("#include \"bar.h\"\na::b::bar b;\n",
            runIncludeFixer("b::bar b;\n"));
  EXPECT_EQ("#include \"bar.h\"\na::b::bar b;\n",
            runIncludeFixer("a::b::bar b;\n"));
  EXPECT_EQ("#include \"bar.h\"\na::b::bar b;\n",
            runIncludeFixer("bar b;\n"));
  EXPECT_EQ("#include \"bar.h\"\nnamespace a {\nb::bar b;\n}\n",
            runIncludeFixer("namespace a {\nb::bar b;\n}\n"));
  EXPECT_EQ("#include \"bar.h\"\nnamespace a {\nb::bar b;\n}\n",
            runIncludeFixer("namespace a {\nbar b;\n}\n"));
  EXPECT_EQ("#include \"bar.h\"\nnamespace a {\nnamespace b{\nbar b;\n}\n}\n",
            runIncludeFixer("namespace a {\nnamespace b{\nbar b;\n}\n}\n"));
  EXPECT_EQ("c::b::bar b;\n",
            runIncludeFixer("c::b::bar b;\n"));
  EXPECT_EQ("#include \"bar.h\"\nnamespace c {\na::b::bar b;\n}\n",
            runIncludeFixer("namespace c {\nbar b;\n}\n"));

  // Test nested classes.
  EXPECT_EQ("#include \"bar.h\"\nnamespace c {\na::b::bar::t b;\n}\n",
            runIncludeFixer("namespace c {\nbar::t b;\n}\n"));
  EXPECT_EQ("#include \"bar.h\"\nnamespace a {\nb::bar::t b;\n}\n",
            runIncludeFixer("namespace a {\nbar::t b;\n}\n"));

  EXPECT_EQ(
      "#include \"color.h\"\nint test = a::b::Green;\n",
      runIncludeFixer("int test = Green;\n"));
  EXPECT_EQ("#include \"color.h\"\nnamespace d {\nint test = a::b::Green;\n}\n",
            runIncludeFixer("namespace d {\nint test = Green;\n}\n"));
  EXPECT_EQ("#include \"color.h\"\nnamespace a {\nint test = b::Green;\n}\n",
            runIncludeFixer("namespace a {\nint test = Green;\n}\n"));

  // FIXME: Fix-namespace should not fix the global qualified identifier.
  EXPECT_EQ(
      "#include \"bar.h\"\na::b::bar b;\n",
      runIncludeFixer("::a::b::bar b;\n"));
}

} // namespace
} // namespace include_fixer
} // namespace clang
