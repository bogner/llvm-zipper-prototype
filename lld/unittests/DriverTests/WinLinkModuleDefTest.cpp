//===- lld/unittest/WinLinkModuleDefTest.cpp ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <memory>

#include "gtest/gtest.h"
#include "lld/Driver/WinLinkModuleDef.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace lld;

class ParserTest : public testing::Test {
protected:
  bool parse(const char *contents,
             std::vector<PECOFFLinkingContext::ExportDesc> &ret) {
    auto membuf =
        std::unique_ptr<MemoryBuffer>(MemoryBuffer::getMemBuffer(contents));
    moduledef::Lexer lexer(std::move(membuf));
    moduledef::Parser parser(lexer);
    return parser.parse(ret);
  }
};

TEST_F(ParserTest, Exports) {
  std::vector<PECOFFLinkingContext::ExportDesc> exports;
  EXPECT_TRUE(parse("EXPORTS\n"
                    "  sym1\n"
                    "  sym2 @5\n"
                    "  sym3 @8 NONAME\n"
                    "  sym4 DATA\n"
                    "  sym5 @10 NONAME DATA\n",
                    exports));
  EXPECT_EQ(5U, exports.size());
  EXPECT_EQ(exports[0].name, "sym1");
  EXPECT_EQ(exports[0].ordinal, -1);
  EXPECT_EQ(exports[0].noname, false);
  EXPECT_EQ(exports[0].isData, false);
  EXPECT_EQ(exports[1].name, "sym2");
  EXPECT_EQ(exports[1].ordinal, 5);
  EXPECT_EQ(exports[1].noname, false);
  EXPECT_EQ(exports[1].isData, false);
  EXPECT_EQ(exports[2].name, "sym3");
  EXPECT_EQ(exports[2].ordinal, 8);
  EXPECT_EQ(exports[2].noname, true);
  EXPECT_EQ(exports[2].isData, false);
  EXPECT_EQ(exports[3].name, "sym4");
  EXPECT_EQ(exports[3].ordinal, -1);
  EXPECT_EQ(exports[3].noname, false);
  EXPECT_EQ(exports[3].isData, true);
  EXPECT_EQ(exports[4].name, "sym5");
  EXPECT_EQ(exports[4].ordinal, 10);
  EXPECT_EQ(exports[4].noname, true);
  EXPECT_EQ(exports[4].isData, true);
}
