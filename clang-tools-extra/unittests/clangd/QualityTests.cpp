//===-- SourceCodeTests.cpp  ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Evaluating scoring functions isn't a great fit for assert-based tests.
// For interesting cases, both exact scores and "X beats Y" are too brittle to
// make good hard assertions.
//
// Here we test the signal extraction and sanity-check that signals point in
// the right direction. This should be supplemented by quality metrics which
// we can compute from a corpus of queries and preferred rankings.
//
//===----------------------------------------------------------------------===//

#include "Quality.h"
#include "TestTU.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace clang {
namespace clangd {
namespace {

TEST(QualityTests, SymbolQualitySignalExtraction) {
  auto Header = TestTU::withHeaderCode(R"cpp(
    int x;

    [[deprecated]]
    int f() { return x; }
  )cpp");
  auto Symbols = Header.headerSymbols();
  auto AST = Header.build();

  SymbolQualitySignals Quality;
  Quality.merge(findSymbol(Symbols, "x"));
  EXPECT_FALSE(Quality.Deprecated);
  EXPECT_EQ(Quality.SemaCCPriority, SymbolQualitySignals().SemaCCPriority);
  EXPECT_EQ(Quality.References, SymbolQualitySignals().References);

  Symbol F = findSymbol(Symbols, "f");
  F.References = 24; // TestTU doesn't count references, so fake it.
  Quality = {};
  Quality.merge(F);
  EXPECT_FALSE(Quality.Deprecated); // FIXME: Include deprecated bit in index.
  EXPECT_EQ(Quality.SemaCCPriority, SymbolQualitySignals().SemaCCPriority);
  EXPECT_EQ(Quality.References, 24u);

  Quality = {};
  Quality.merge(CodeCompletionResult(&findDecl(AST, "f"), /*Priority=*/42));
  EXPECT_TRUE(Quality.Deprecated);
  EXPECT_EQ(Quality.SemaCCPriority, 42u);
  EXPECT_EQ(Quality.References, SymbolQualitySignals().References);
}

TEST(QualityTests, SymbolRelevanceSignalExtraction) {
  TestTU Test;
  Test.HeaderCode = R"cpp(
    int test_func_in_header();
    int test_func_in_header_and_cpp();
    )cpp";
  Test.Code = R"cpp(
    int ::test_func_in_header_and_cpp() {
    }
    int test_func_in_cpp();

    [[deprecated]]
    int test_deprecated() { return 0; }
  )cpp";
  auto AST = Test.build();

  SymbolRelevanceSignals Deprecated;
  Deprecated.merge(CodeCompletionResult(&findDecl(AST, "test_deprecated"),
                                        /*Priority=*/42, nullptr, false,
                                        /*Accessible=*/false));
  EXPECT_EQ(Deprecated.NameMatch, SymbolRelevanceSignals().NameMatch);
  EXPECT_TRUE(Deprecated.Forbidden);

  // Test proximity scores.
  SymbolRelevanceSignals FuncInCpp;
  FuncInCpp.merge(CodeCompletionResult(&findDecl(AST, "test_func_in_cpp"),
                                       CCP_Declaration));
  /// Decls in the current file should get a proximity score of 1.0.
  EXPECT_FLOAT_EQ(FuncInCpp.ProximityScore, 1.0);

  SymbolRelevanceSignals FuncInHeader;
  FuncInHeader.merge(CodeCompletionResult(&findDecl(AST, "test_func_in_header"),
                                          CCP_Declaration));
  /// Decls outside current file currently don't get a proximity score boost.
  EXPECT_FLOAT_EQ(FuncInHeader.ProximityScore, 0.0);

  SymbolRelevanceSignals FuncInHeaderAndCpp;
  FuncInHeaderAndCpp.merge(CodeCompletionResult(
      &findDecl(AST, "test_func_in_header_and_cpp"), CCP_Declaration));
  /// Decls in both header **and** the main file get the same boost.
  EXPECT_FLOAT_EQ(FuncInHeaderAndCpp.ProximityScore, 1.0);
}

// Do the signals move the scores in the direction we expect?
TEST(QualityTests, SymbolQualitySignalsSanity) {
  SymbolQualitySignals Default;
  EXPECT_EQ(Default.evaluate(), 1);

  SymbolQualitySignals Deprecated;
  Deprecated.Deprecated = true;
  EXPECT_LT(Deprecated.evaluate(), Default.evaluate());

  SymbolQualitySignals WithReferences, ManyReferences;
  WithReferences.References = 10;
  ManyReferences.References = 1000;
  EXPECT_GT(WithReferences.evaluate(), Default.evaluate());
  EXPECT_GT(ManyReferences.evaluate(), WithReferences.evaluate());

  SymbolQualitySignals LowPriority, HighPriority;
  LowPriority.SemaCCPriority = 60;
  HighPriority.SemaCCPriority = 20;
  EXPECT_GT(HighPriority.evaluate(), Default.evaluate());
  EXPECT_LT(LowPriority.evaluate(), Default.evaluate());
}

TEST(QualityTests, SymbolRelevanceSignalsSanity) {
  SymbolRelevanceSignals Default;
  EXPECT_EQ(Default.evaluate(), 1);

  SymbolRelevanceSignals Forbidden;
  Forbidden.Forbidden = true;
  EXPECT_LT(Forbidden.evaluate(), Default.evaluate());

  SymbolRelevanceSignals PoorNameMatch;
  PoorNameMatch.NameMatch = 0.2f;
  EXPECT_LT(PoorNameMatch.evaluate(), Default.evaluate());

  SymbolRelevanceSignals WithProximity;
  WithProximity.ProximityScore = 0.2;
  EXPECT_LT(Default.evaluate(), WithProximity.evaluate());
}

TEST(QualityTests, SortText) {
  EXPECT_LT(sortText(std::numeric_limits<float>::infinity()), sortText(1000.2f));
  EXPECT_LT(sortText(1000.2f), sortText(1));
  EXPECT_LT(sortText(1), sortText(0.3f));
  EXPECT_LT(sortText(0.3f), sortText(0));
  EXPECT_LT(sortText(0), sortText(-10));
  EXPECT_LT(sortText(-10), sortText(-std::numeric_limits<float>::infinity()));

  EXPECT_LT(sortText(1, "z"), sortText(0, "a"));
  EXPECT_LT(sortText(0, "a"), sortText(0, "z"));
}

} // namespace
} // namespace clangd
} // namespace clang
