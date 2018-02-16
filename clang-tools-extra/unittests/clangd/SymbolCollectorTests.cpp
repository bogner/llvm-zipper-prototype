//===-- SymbolCollectorTests.cpp  -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Annotations.h"
#include "TestFS.h"
#include "index/SymbolCollector.h"
#include "index/SymbolYAML.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Basic/VirtualFileSystem.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <memory>
#include <string>

using testing::AllOf;
using testing::Eq;
using testing::Field;
using testing::Not;
using testing::UnorderedElementsAre;
using testing::UnorderedElementsAreArray;

// GMock helpers for matching Symbol.
MATCHER_P(Labeled, Label, "") { return arg.CompletionLabel == Label; }
MATCHER(HasDetail, "") { return arg.Detail; }
MATCHER_P(Detail, D, "") {
  return arg.Detail && arg.Detail->CompletionDetail == D;
}
MATCHER_P(Doc, D, "") { return arg.Detail && arg.Detail->Documentation == D; }
MATCHER_P(Plain, Text, "") { return arg.CompletionPlainInsertText == Text; }
MATCHER_P(Snippet, S, "") {
  return arg.CompletionSnippetInsertText == S;
}
MATCHER_P(QName, Name, "") { return (arg.Scope + arg.Name).str() == Name; }
MATCHER_P(DeclURI, P, "") { return arg.CanonicalDeclaration.FileURI == P; }
MATCHER_P(DeclRange, Offsets, "") {
  return arg.CanonicalDeclaration.StartOffset == Offsets.first &&
      arg.CanonicalDeclaration.EndOffset == Offsets.second;
}
MATCHER_P(DefRange, Offsets, "") {
  return arg.Definition.StartOffset == Offsets.first &&
         arg.Definition.EndOffset == Offsets.second;
}

namespace clang {
namespace clangd {

namespace {
class SymbolIndexActionFactory : public tooling::FrontendActionFactory {
public:
  SymbolIndexActionFactory(SymbolCollector::Options COpts)
      : COpts(std::move(COpts)) {}

  clang::FrontendAction *create() override {
    index::IndexingOptions IndexOpts;
    IndexOpts.SystemSymbolFilter =
        index::IndexingOptions::SystemSymbolFilterKind::All;
    IndexOpts.IndexFunctionLocals = false;
    Collector = std::make_shared<SymbolCollector>(COpts);
    FrontendAction *Action =
        index::createIndexingAction(Collector, IndexOpts, nullptr).release();
    return Action;
  }

  std::shared_ptr<SymbolCollector> Collector;
  SymbolCollector::Options COpts;
};

class SymbolCollectorTest : public ::testing::Test {
public:
  SymbolCollectorTest()
      : TestHeaderName(testPath("symbol.h")),
        TestFileName(testPath("symbol.cc")) {
    TestHeaderURI = URI::createFile(TestHeaderName).toString();
    TestFileURI = URI::createFile(TestFileName).toString();
  }

  bool runSymbolCollector(StringRef HeaderCode, StringRef MainCode,
                          const std::vector<std::string> &ExtraArgs = {}) {
    llvm::IntrusiveRefCntPtr<vfs::InMemoryFileSystem> InMemoryFileSystem(
        new vfs::InMemoryFileSystem);
    llvm::IntrusiveRefCntPtr<FileManager> Files(
        new FileManager(FileSystemOptions(), InMemoryFileSystem));

    auto Factory = llvm::make_unique<SymbolIndexActionFactory>(CollectorOpts);

    std::vector<std::string> Args = {"symbol_collector", "-fsyntax-only",
                                     "-std=c++11",       "-include",
                                     TestHeaderName,     TestFileName};
    Args.insert(Args.end(), ExtraArgs.begin(), ExtraArgs.end());
    tooling::ToolInvocation Invocation(
        Args,
        Factory->create(), Files.get(),
        std::make_shared<PCHContainerOperations>());

    InMemoryFileSystem->addFile(TestHeaderName, 0,
                                llvm::MemoryBuffer::getMemBuffer(HeaderCode));
    InMemoryFileSystem->addFile(TestFileName, 0,
                                llvm::MemoryBuffer::getMemBuffer(MainCode));
    Invocation.run();
    Symbols = Factory->Collector->takeSymbols();
    return true;
  }

protected:
  std::string TestHeaderName;
  std::string TestHeaderURI;
  std::string TestFileName;
  std::string TestFileURI;
  SymbolSlab Symbols;
  SymbolCollector::Options CollectorOpts;
};

TEST_F(SymbolCollectorTest, CollectSymbols) {
  CollectorOpts.IndexMainFiles = true;
  const std::string Header = R"(
    class Foo {
      void f();
    };
    void f1();
    inline void f2() {}
    static const int KInt = 2;
    const char* kStr = "123";
  )";
  const std::string Main = R"(
    namespace {
    void ff() {} // ignore
    }

    void f1() {}

    namespace foo {
    // Type alias
    typedef int int32;
    using int32_t = int32;

    // Variable
    int v1;

    // Namespace
    namespace bar {
    int v2;
    }
    // Namespace alias
    namespace baz = bar;

    // FIXME: using declaration is not supported as the IndexAction will ignore
    // implicit declarations (the implicit using shadow declaration) by default,
    // and there is no way to customize this behavior at the moment.
    using bar::v2;
    } // namespace foo
  )";
  runSymbolCollector(Header, Main);
  EXPECT_THAT(Symbols,
              UnorderedElementsAreArray(
                  {QName("Foo"), QName("f1"), QName("f2"), QName("KInt"),
                   QName("kStr"), QName("foo"), QName("foo::bar"),
                   QName("foo::int32"), QName("foo::int32_t"), QName("foo::v1"),
                   QName("foo::bar::v2"), QName("foo::baz")}));
}

TEST_F(SymbolCollectorTest, Locations) {
  CollectorOpts.IndexMainFiles = true;
  Annotations Header(R"cpp(
    // Declared in header, defined in main.
    extern int $xdecl[[X]];
    class $clsdecl[[Cls]];
    void $printdecl[[print]]();

    // Declared in header, defined nowhere.
    extern int $zdecl[[Z]];
  )cpp");
  Annotations Main(R"cpp(
    int $xdef[[X]] = 42;
    class $clsdef[[Cls]] {};
    void $printdef[[print]]() {}

    // Declared/defined in main only.
    int $y[[Y]];
  )cpp");
  runSymbolCollector(Header.code(), Main.code());
  EXPECT_THAT(
      Symbols,
      UnorderedElementsAre(
          AllOf(QName("X"), DeclRange(Header.offsetRange("xdecl")),
                DefRange(Main.offsetRange("xdef"))),
          AllOf(QName("Cls"), DeclRange(Header.offsetRange("clsdecl")),
                DefRange(Main.offsetRange("clsdef"))),
          AllOf(QName("print"), DeclRange(Header.offsetRange("printdecl")),
                DefRange(Main.offsetRange("printdef"))),
          AllOf(QName("Z"), DeclRange(Header.offsetRange("zdecl"))),
          AllOf(QName("Y"), DeclRange(Main.offsetRange("y")),
                DefRange(Main.offsetRange("y")))));
}

TEST_F(SymbolCollectorTest, SymbolRelativeNoFallback) {
  CollectorOpts.IndexMainFiles = false;
  runSymbolCollector("class Foo {};", /*Main=*/"");
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(AllOf(QName("Foo"), DeclURI(TestHeaderURI))));
}

TEST_F(SymbolCollectorTest, SymbolRelativeWithFallback) {
  CollectorOpts.IndexMainFiles = false;
  TestHeaderName = "x.h";
  TestFileName = "x.cpp";
  TestHeaderURI = URI::createFile(testPath(TestHeaderName)).toString();
  CollectorOpts.FallbackDir = testRoot();
  runSymbolCollector("class Foo {};", /*Main=*/"");
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(AllOf(QName("Foo"), DeclURI(TestHeaderURI))));
}

#ifndef LLVM_ON_WIN32
TEST_F(SymbolCollectorTest, CustomURIScheme) {
  CollectorOpts.IndexMainFiles = false;
  // Use test URI scheme from URITests.cpp
  CollectorOpts.URISchemes.insert(CollectorOpts.URISchemes.begin(), "unittest");
  TestHeaderName = testPath("test-root/x.h");
  TestFileName = testPath("test-root/x.cpp");
  runSymbolCollector("class Foo {};", /*Main=*/"");
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(AllOf(QName("Foo"), DeclURI("unittest:x.h"))));
}
#endif

TEST_F(SymbolCollectorTest, InvalidURIScheme) {
  CollectorOpts.IndexMainFiles = false;
  // Use test URI scheme from URITests.cpp
  CollectorOpts.URISchemes = {"invalid"};
  runSymbolCollector("class Foo {};", /*Main=*/"");
  EXPECT_THAT(Symbols, UnorderedElementsAre(AllOf(QName("Foo"), DeclURI(""))));
}

TEST_F(SymbolCollectorTest, FallbackToFileURI) {
  CollectorOpts.IndexMainFiles = false;
  // Use test URI scheme from URITests.cpp
  CollectorOpts.URISchemes = {"invalid", "file"};
  runSymbolCollector("class Foo {};", /*Main=*/"");
  EXPECT_THAT(Symbols, UnorderedElementsAre(
                           AllOf(QName("Foo"), DeclURI(TestHeaderURI))));
}

TEST_F(SymbolCollectorTest, IncludeEnums) {
  CollectorOpts.IndexMainFiles = false;
  const std::string Header = R"(
    enum {
      Red
    };
    enum Color {
      Green
    };
    enum class Color2 {
      Yellow // ignore
    };
    namespace ns {
    enum {
      Black
    };
    }
  )";
  runSymbolCollector(Header, /*Main=*/"");
  EXPECT_THAT(Symbols, UnorderedElementsAre(QName("Red"), QName("Color"),
                                            QName("Green"), QName("Color2"),
                                            QName("ns"), QName("ns::Black")));
}

TEST_F(SymbolCollectorTest, IgnoreNamelessSymbols) {
  CollectorOpts.IndexMainFiles = false;
  const std::string Header = R"(
    struct {
      int a;
    } Foo;
  )";
  runSymbolCollector(Header, /*Main=*/"");
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(QName("Foo")));
}

TEST_F(SymbolCollectorTest, SymbolFormedFromMacro) {
  CollectorOpts.IndexMainFiles = false;

  Annotations Header(R"(
    #define FF(name) \
      class name##_Test {};

    $expansion[[FF]](abc);

    #define FF2() \
      class $spelling[[Test]] {};

    FF2();
  )");

  runSymbolCollector(Header.code(), /*Main=*/"");
  EXPECT_THAT(
      Symbols,
      UnorderedElementsAre(
          AllOf(QName("abc_Test"), DeclRange(Header.offsetRange("expansion")),
                DeclURI(TestHeaderURI)),
          AllOf(QName("Test"), DeclRange(Header.offsetRange("spelling")),
                DeclURI(TestHeaderURI))));
}

TEST_F(SymbolCollectorTest, SymbolFormedFromMacroInMainFile) {
  CollectorOpts.IndexMainFiles = true;

  Annotations Main(R"(
    #define FF(name) \
      class name##_Test {};

    $expansion[[FF]](abc);

    #define FF2() \
      class $spelling[[Test]] {};

    FF2();
  )");
  runSymbolCollector(/*Header=*/"", Main.code());
  EXPECT_THAT(
      Symbols,
      UnorderedElementsAre(
          AllOf(QName("abc_Test"), DeclRange(Main.offsetRange("expansion")),
                DeclURI(TestFileURI)),
          AllOf(QName("Test"), DeclRange(Main.offsetRange("spelling")),
                DeclURI(TestFileURI))));
}

TEST_F(SymbolCollectorTest, SymbolFormedByCLI) {
  CollectorOpts.IndexMainFiles = false;

  Annotations Header(R"(
    #ifdef NAME
    class $expansion[[NAME]] {};
    #endif
  )");

  runSymbolCollector(Header.code(), /*Main=*/"",
                     /*ExtraArgs=*/{"-DNAME=name"});
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(AllOf(
                  QName("name"), DeclRange(Header.offsetRange("expansion")),
                  DeclURI(TestHeaderURI))));
}

TEST_F(SymbolCollectorTest, IgnoreSymbolsInMainFile) {
  CollectorOpts.IndexMainFiles = false;
  const std::string Header = R"(
    class Foo {};
    void f1();
    inline void f2() {}
  )";
  const std::string Main = R"(
    namespace {
    void ff() {} // ignore
    }
    void main_f() {} // ignore
    void f1() {}
  )";
  runSymbolCollector(Header, Main);
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(QName("Foo"), QName("f1"), QName("f2")));
}

TEST_F(SymbolCollectorTest, IncludeSymbolsInMainFile) {
  CollectorOpts.IndexMainFiles = true;
  const std::string Header = R"(
    class Foo {};
    void f1();
    inline void f2() {}
  )";
  const std::string Main = R"(
    namespace {
    void ff() {} // ignore
    }
    void main_f() {}
    void f1() {}
  )";
  runSymbolCollector(Header, Main);
  EXPECT_THAT(Symbols, UnorderedElementsAre(QName("Foo"), QName("f1"),
                                            QName("f2"), QName("main_f")));
}

TEST_F(SymbolCollectorTest, IgnoreClassMembers) {
  const std::string Header = R"(
    class Foo {
      void f() {}
      void g();
      static void sf() {}
      static void ssf();
      static int x;
    };
  )";
  const std::string Main = R"(
    void Foo::g() {}
    void Foo::ssf() {}
  )";
  runSymbolCollector(Header, Main);
  EXPECT_THAT(Symbols, UnorderedElementsAre(QName("Foo")));
}

TEST_F(SymbolCollectorTest, Scopes) {
  const std::string Header = R"(
    namespace na {
    class Foo {};
    namespace nb {
    class Bar {};
    }
    }
  )";
  runSymbolCollector(Header, /*Main=*/"");
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(QName("na"), QName("na::nb"),
                                   QName("na::Foo"), QName("na::nb::Bar")));
}

TEST_F(SymbolCollectorTest, ExternC) {
  const std::string Header = R"(
    extern "C" { class Foo {}; }
    namespace na {
    extern "C" { class Bar {}; }
    }
  )";
  runSymbolCollector(Header, /*Main=*/"");
  EXPECT_THAT(Symbols, UnorderedElementsAre(QName("na"), QName("Foo"),
                                            QName("na::Bar")));
}

TEST_F(SymbolCollectorTest, SkipInlineNamespace) {
  const std::string Header = R"(
    namespace na {
    inline namespace nb {
    class Foo {};
    }
    }
    namespace na {
    // This is still inlined.
    namespace nb {
    class Bar {};
    }
    }
  )";
  runSymbolCollector(Header, /*Main=*/"");
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(QName("na"), QName("na::nb"),
                                   QName("na::Foo"), QName("na::Bar")));
}

TEST_F(SymbolCollectorTest, SymbolWithDocumentation) {
  const std::string Header = R"(
    namespace nx {
    /// Foo comment.
    int ff(int x, double y) { return 0; }
    }
  )";
  runSymbolCollector(Header, /*Main=*/"");
  EXPECT_THAT(Symbols,
              UnorderedElementsAre(QName("nx"),
                                   AllOf(QName("nx::ff"),
                                         Labeled("ff(int x, double y)"),
                                         Detail("int"), Doc("Foo comment."))));
}

TEST_F(SymbolCollectorTest, PlainAndSnippet) {
  const std::string Header = R"(
    namespace nx {
    void f() {}
    int ff(int x, double y) { return 0; }
    }
  )";
  runSymbolCollector(Header, /*Main=*/"");
  EXPECT_THAT(
      Symbols,
      UnorderedElementsAre(
          QName("nx"),
          AllOf(QName("nx::f"), Labeled("f()"), Plain("f"), Snippet("f()")),
          AllOf(QName("nx::ff"), Labeled("ff(int x, double y)"), Plain("ff"),
                Snippet("ff(${1:int x}, ${2:double y})"))));
}

TEST_F(SymbolCollectorTest, YAMLConversions) {
  const std::string YAML1 = R"(
---
ID: 057557CEBF6E6B2DD437FBF60CC58F352D1DF856
Name:   'Foo1'
Scope:   'clang::'
SymInfo:
  Kind:            Function
  Lang:            Cpp
CanonicalDeclaration:
  StartOffset:     0
  EndOffset:       1
  FileURI:        file:///path/foo.h
CompletionLabel:    'Foo1-label'
CompletionFilterText:    'filter'
CompletionPlainInsertText:    'plain'
Detail:
  Documentation:    'Foo doc'
  CompletionDetail:    'int'
...
)";
  const std::string YAML2 = R"(
---
ID: 057557CEBF6E6B2DD437FBF60CC58F352D1DF858
Name:   'Foo2'
Scope:   'clang::'
SymInfo:
  Kind:            Function
  Lang:            Cpp
CanonicalDeclaration:
  StartOffset:     10
  EndOffset:       12
  FileURI:        file:///path/bar.h
CompletionLabel:    'Foo2-label'
CompletionFilterText:    'filter'
CompletionPlainInsertText:    'plain'
CompletionSnippetInsertText:    'snippet'
...
)";

  auto Symbols1 = SymbolsFromYAML(YAML1);
  EXPECT_THAT(Symbols1,
              UnorderedElementsAre(AllOf(
                  QName("clang::Foo1"), Labeled("Foo1-label"), Doc("Foo doc"),
                  Detail("int"), DeclURI("file:///path/foo.h"))));
  auto Symbols2 = SymbolsFromYAML(YAML2);
  EXPECT_THAT(Symbols2, UnorderedElementsAre(AllOf(
                            QName("clang::Foo2"), Labeled("Foo2-label"),
                            Not(HasDetail()), DeclURI("file:///path/bar.h"))));

  std::string ConcatenatedYAML;
  {
    llvm::raw_string_ostream OS(ConcatenatedYAML);
    SymbolsToYAML(Symbols1, OS);
    SymbolsToYAML(Symbols2, OS);
  }
  auto ConcatenatedSymbols = SymbolsFromYAML(ConcatenatedYAML);
  EXPECT_THAT(ConcatenatedSymbols,
              UnorderedElementsAre(QName("clang::Foo1"),
                                   QName("clang::Foo2")));
}

} // namespace
} // namespace clangd
} // namespace clang
