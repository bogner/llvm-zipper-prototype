//===- lld/unittest/GnuLdDriverTest.cpp -----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief GNU ld driver tests.
///
//===----------------------------------------------------------------------===//

#include "DriverTest.h"

#include "lld/ReaderWriter/ELFLinkingContext.h"

using namespace llvm;
using namespace lld;

namespace {
class GnuLdParserTest
    : public ParserTest<GnuLdDriver, std::unique_ptr<ELFLinkingContext>> {
protected:
  const LinkingContext *linkingContext() override { return _context.get(); }
};
}

// All calls of parse() in this file has empty "--start-group" and "--end-group"
// options. This is a workaround for the current GNU-compatible driver. The
// driver complains if no input file is given, but if we give a file, it tries
// to read it to get magic bytes. It's not suitable for unit tests.
//
// TODO: Modify the driver to make it more test friendly.

TEST_F(GnuLdParserTest, Empty) {
  EXPECT_FALSE(parse("ld", nullptr));
  EXPECT_EQ(linkingContext(), nullptr);
  EXPECT_EQ("No input files\n", errorMessage());
}

// --soname

TEST_F(GnuLdParserTest, SOName) {
  EXPECT_TRUE(parse("ld", "--start-group", "--end-group", "--soname=foo",
                    nullptr));
  EXPECT_EQ("foo", _context->sharedObjectName());
}

TEST_F(GnuLdParserTest, SONameSingleDash) {
  EXPECT_TRUE(parse("ld", "--start-group", "--end-group", "-soname=foo",
                    nullptr));
  EXPECT_EQ("foo", _context->sharedObjectName());
}

TEST_F(GnuLdParserTest, SONameH) {
  EXPECT_TRUE(parse("ld", "--start-group", "--end-group", "-h", "foo",
                    nullptr));
  EXPECT_EQ("foo", _context->sharedObjectName());
}

// -rpath

TEST_F(GnuLdParserTest, Rpath) {
  EXPECT_TRUE(parse("ld", "--start-group", "--end-group", "-rpath", "foo:bar",
                    nullptr));
  EXPECT_EQ(2, _context->getRpathList().size());
  EXPECT_EQ("foo", _context->getRpathList()[0]);
  EXPECT_EQ("bar", _context->getRpathList()[1]);
}

// --defsym

TEST_F(GnuLdParserTest, DefsymDecimal) {
  EXPECT_TRUE(parse("ld", "--start-group", "--end-group", "--defsym=sym=1000",
                    nullptr));
  assert(_context.get());
  auto map = _context->getAbsoluteSymbols();
  EXPECT_EQ((size_t)1, map.size());
  EXPECT_EQ((uint64_t)1000, map["sym"]);
}

TEST_F(GnuLdParserTest, DefsymHexadecimal) {
  EXPECT_TRUE(parse("ld", "--start-group", "--end-group", "--defsym=sym=0x1000",
                    nullptr));
  auto map = _context->getAbsoluteSymbols();
  EXPECT_EQ((size_t)1, map.size());
  EXPECT_EQ((uint64_t)0x1000, map["sym"]);
}

TEST_F(GnuLdParserTest, DefsymOctal) {
  EXPECT_TRUE(parse("ld", "--start-group", "--end-group", "--defsym=sym=0777",
                    nullptr));
  auto map = _context->getAbsoluteSymbols();
  EXPECT_EQ((size_t)1, map.size());
  EXPECT_EQ((uint64_t)0777, map["sym"]);
}

TEST_F(GnuLdParserTest, DefsymFail) {
  EXPECT_FALSE(
      parse("ld", "--start-group", "--end-group", "--defsym=sym=abc", nullptr));
}

TEST_F(GnuLdParserTest, DefsymMisssingSymbol) {
  EXPECT_FALSE(
      parse("ld", "--start-group", "--end-group", "--defsym==0", nullptr));
}

TEST_F(GnuLdParserTest, DefsymMisssingValue) {
  EXPECT_FALSE(
      parse("ld", "--start-group", "--end-group", "--defsym=sym=", nullptr));
}
