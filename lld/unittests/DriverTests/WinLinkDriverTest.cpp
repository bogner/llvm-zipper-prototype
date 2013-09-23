//===- lld/unittest/WinLinkDriverTest.cpp ---------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Windows link.exe driver tests.
///
//===----------------------------------------------------------------------===//

#include "DriverTest.h"

#include "lld/ReaderWriter/PECOFFLinkingContext.h"
#include "llvm/Support/COFF.h"

#include <vector>

using namespace llvm;
using namespace lld;

namespace {

class WinLinkParserTest
    : public ParserTest<WinLinkDriver, PECOFFLinkingContext> {
protected:
  virtual const LinkingContext *linkingContext() { return &_context; }
};

TEST_F(WinLinkParserTest, Basic) {
  EXPECT_FALSE(parse("link.exe", "/subsystem:console", "/out:a.exe",
        "-entry:start", "a.obj", "b.obj", "c.obj", nullptr));
  EXPECT_EQ(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI, _context.getSubsystem());
  EXPECT_EQ(llvm::COFF::IMAGE_FILE_MACHINE_I386, _context.getMachineType());
  EXPECT_EQ("a.exe", _context.outputPath());
  EXPECT_EQ("_start", _context.entrySymbolName());
  EXPECT_EQ(3, inputFileCount());
  EXPECT_EQ("a.obj", inputFile(0));
  EXPECT_EQ("b.obj", inputFile(1));
  EXPECT_EQ("c.obj", inputFile(2));
  EXPECT_TRUE(_context.getInputSearchPaths().empty());

  // Unspecified flags will have default values.
  EXPECT_EQ(6, _context.getMinOSVersion().majorVersion);
  EXPECT_EQ(0, _context.getMinOSVersion().minorVersion);
  EXPECT_EQ(0x400000U, _context.getBaseAddress());
  EXPECT_EQ(1024 * 1024U, _context.getStackReserve());
  EXPECT_EQ(4096U, _context.getStackCommit());
  EXPECT_EQ(4096U, _context.getSectionAlignment());
  EXPECT_FALSE(_context.allowRemainingUndefines());
  EXPECT_TRUE(_context.isNxCompat());
  EXPECT_FALSE(_context.getLargeAddressAware());
  EXPECT_TRUE(_context.getAllowBind());
  EXPECT_TRUE(_context.getAllowIsolation());
  EXPECT_TRUE(_context.getBaseRelocationEnabled());
  EXPECT_TRUE(_context.isTerminalServerAware());
  EXPECT_TRUE(_context.getDynamicBaseEnabled());
  EXPECT_TRUE(_context.deadStrip());
}

TEST_F(WinLinkParserTest, UnixStyleOption) {
  EXPECT_FALSE(parse("link.exe", "-subsystem", "console", "-out", "a.exe",
                     "a.obj", nullptr));
  EXPECT_EQ(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI, _context.getSubsystem());
  EXPECT_EQ("a.exe", _context.outputPath());
  EXPECT_EQ(1, inputFileCount());
  EXPECT_EQ("a.obj", inputFile(0));
}

TEST_F(WinLinkParserTest, UppercaseOption) {
  EXPECT_FALSE(parse("link.exe", "/SUBSYSTEM:CONSOLE", "/OUT:a.exe", "a.obj",
                     nullptr));
  EXPECT_EQ(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI, _context.getSubsystem());
  EXPECT_EQ("a.exe", _context.outputPath());
  EXPECT_EQ(1, inputFileCount());
  EXPECT_EQ("a.obj", inputFile(0));
}

TEST_F(WinLinkParserTest, Mllvm) {
  EXPECT_FALSE(parse("link.exe", "-mllvm", "-debug", "a.obj", nullptr));
  const std::vector<const char *> &options = _context.llvmOptions();
  EXPECT_EQ(1U, options.size());
  EXPECT_EQ("-debug", options[0]);
}

TEST_F(WinLinkParserTest, NoFileExtension) {
  EXPECT_FALSE(parse("link.exe", "foo", "bar", nullptr));
  EXPECT_EQ("foo.exe", _context.outputPath());
  EXPECT_EQ(2, inputFileCount());
  EXPECT_EQ("foo.obj", inputFile(0));
  EXPECT_EQ("bar.obj", inputFile(1));
}

TEST_F(WinLinkParserTest, NonStandardFileExtension) {
  EXPECT_FALSE(parse("link.exe", "foo.o", nullptr));
  EXPECT_EQ("foo.exe", _context.outputPath());
  EXPECT_EQ(1, inputFileCount());
  EXPECT_EQ("foo.o", inputFile(0));
}

TEST_F(WinLinkParserTest, Libpath) {
  EXPECT_FALSE(parse("link.exe", "/libpath:dir1", "/libpath:dir2",
                     "a.obj", nullptr));
  const std::vector<StringRef> &paths = _context.getInputSearchPaths();
  EXPECT_EQ(2U, paths.size());
  EXPECT_EQ("dir1", paths[0]);
  EXPECT_EQ("dir2", paths[1]);
}

TEST_F(WinLinkParserTest, MachineX64) {
  EXPECT_TRUE(parse("link.exe", "/machine:x64", "a.obj", nullptr));
}

TEST_F(WinLinkParserTest, MajorImageVersion) {
  EXPECT_FALSE(parse("link.exe", "/version:7", "foo.o", nullptr));
  EXPECT_EQ(7, _context.getImageVersion().majorVersion);
  EXPECT_EQ(0, _context.getImageVersion().minorVersion);
}

TEST_F(WinLinkParserTest, MajorMinorImageVersion) {
  EXPECT_FALSE(parse("link.exe", "/version:72.35", "foo.o", nullptr));
  EXPECT_EQ(72, _context.getImageVersion().majorVersion);
  EXPECT_EQ(35, _context.getImageVersion().minorVersion);
}

TEST_F(WinLinkParserTest, MinMajorOSVersion) {
  EXPECT_FALSE(parse("link.exe", "/subsystem:windows,3", "foo.o", nullptr));
  EXPECT_EQ(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_GUI, _context.getSubsystem());
  EXPECT_EQ(3, _context.getMinOSVersion().majorVersion);
  EXPECT_EQ(0, _context.getMinOSVersion().minorVersion);
}

TEST_F(WinLinkParserTest, MinMajorMinorOSVersion) {
  EXPECT_FALSE(parse("link.exe", "/subsystem:windows,3.1", "foo.o", nullptr));
  EXPECT_EQ(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_GUI, _context.getSubsystem());
  EXPECT_EQ(3, _context.getMinOSVersion().majorVersion);
  EXPECT_EQ(1, _context.getMinOSVersion().minorVersion);
}

TEST_F(WinLinkParserTest, DefaultLib) {
  EXPECT_FALSE(parse("link.exe", "/defaultlib:user32.lib",
                     "/defaultlib:kernel32", "a.obj", nullptr));
  EXPECT_EQ(3, inputFileCount());
  EXPECT_EQ("a.obj", inputFile(0));
  EXPECT_EQ("user32.lib", inputFile(1));
  EXPECT_EQ("kernel32.lib", inputFile(2));
}

TEST_F(WinLinkParserTest, Base) {
  EXPECT_FALSE(parse("link.exe", "/base:8388608", "a.obj", nullptr));
  EXPECT_EQ(0x800000U, _context.getBaseAddress());
}

TEST_F(WinLinkParserTest, StackReserve) {
  EXPECT_FALSE(parse("link.exe", "/stack:8192", "a.obj", nullptr));
  EXPECT_EQ(8192U, _context.getStackReserve());
  EXPECT_EQ(4096U, _context.getStackCommit());
}

TEST_F(WinLinkParserTest, StackReserveAndCommit) {
  EXPECT_FALSE(parse("link.exe", "/stack:16384,8192", "a.obj", nullptr));
  EXPECT_EQ(16384U, _context.getStackReserve());
  EXPECT_EQ(8192U, _context.getStackCommit());
}

TEST_F(WinLinkParserTest, HeapReserve) {
  EXPECT_FALSE(parse("link.exe", "/heap:8192", "a.obj", nullptr));
  EXPECT_EQ(8192U, _context.getHeapReserve());
  EXPECT_EQ(4096U, _context.getHeapCommit());
}

TEST_F(WinLinkParserTest, HeapReserveAndCommit) {
  EXPECT_FALSE(parse("link.exe", "/heap:16384,8192", "a.obj", nullptr));
  EXPECT_EQ(16384U, _context.getHeapReserve());
  EXPECT_EQ(8192U, _context.getHeapCommit());
}

TEST_F(WinLinkParserTest, SectionAlignment) {
  EXPECT_FALSE(parse("link.exe", "/align:8192", "a.obj", nullptr));
  EXPECT_EQ(8192U, _context.getSectionAlignment());
}

TEST_F(WinLinkParserTest, Force) {
  EXPECT_FALSE(parse("link.exe", "/force", "a.obj", nullptr));
  EXPECT_TRUE(_context.allowRemainingUndefines());
}

TEST_F(WinLinkParserTest, ForceUnresolved) {
  EXPECT_FALSE(parse("link.exe", "/force:unresolved", "a.obj", nullptr));
  EXPECT_TRUE(_context.allowRemainingUndefines());
}

TEST_F(WinLinkParserTest, NoNxCompat) {
  EXPECT_FALSE(parse("link.exe", "/nxcompat:no", "a.obj", nullptr));
  EXPECT_FALSE(_context.isNxCompat());
}

TEST_F(WinLinkParserTest, LargeAddressAware) {
  EXPECT_FALSE(parse("link.exe", "/largeaddressaware", "a.obj", nullptr));
  EXPECT_TRUE(_context.getLargeAddressAware());
}

TEST_F(WinLinkParserTest, NoLargeAddressAware) {
  EXPECT_FALSE(parse("link.exe", "/largeaddressaware:no", "a.obj", nullptr));
  EXPECT_FALSE(_context.getLargeAddressAware());
}

TEST_F(WinLinkParserTest, AllowBind) {
  EXPECT_FALSE(parse("link.exe", "/allowbind", "a.obj", nullptr));
  EXPECT_TRUE(_context.getAllowBind());
}

TEST_F(WinLinkParserTest, NoAllowBind) {
  EXPECT_FALSE(parse("link.exe", "/allowbind:no", "a.obj", nullptr));
  EXPECT_FALSE(_context.getAllowBind());
}

TEST_F(WinLinkParserTest, AllowIsolation) {
  EXPECT_FALSE(parse("link.exe", "/allowisolation", "a.obj", nullptr));
  EXPECT_TRUE(_context.getAllowIsolation());
}

TEST_F(WinLinkParserTest, NoAllowIsolation) {
  EXPECT_FALSE(parse("link.exe", "/allowisolation:no", "a.obj", nullptr));
  EXPECT_FALSE(_context.getAllowIsolation());
}

TEST_F(WinLinkParserTest, Fixed) {
  EXPECT_FALSE(parse("link.exe", "/fixed", "a.out", nullptr));
  EXPECT_FALSE(_context.getBaseRelocationEnabled());
  EXPECT_FALSE(_context.getDynamicBaseEnabled());
}

TEST_F(WinLinkParserTest, NoFixed) {
  EXPECT_FALSE(parse("link.exe", "/fixed:no", "a.out", nullptr));
  EXPECT_TRUE(_context.getBaseRelocationEnabled());
}

TEST_F(WinLinkParserTest, TerminalServerAware) {
  EXPECT_FALSE(parse("link.exe", "/tsaware", "a.out", nullptr));
  EXPECT_TRUE(_context.isTerminalServerAware());
}

TEST_F(WinLinkParserTest, NoTerminalServerAware) {
  EXPECT_FALSE(parse("link.exe", "/tsaware:no", "a.out", nullptr));
  EXPECT_FALSE(_context.isTerminalServerAware());
}

TEST_F(WinLinkParserTest, DynamicBase) {
  EXPECT_FALSE(parse("link.exe", "/dynamicbase", "a.out", nullptr));
  EXPECT_TRUE(_context.getDynamicBaseEnabled());
}

TEST_F(WinLinkParserTest, NoDynamicBase) {
  EXPECT_FALSE(parse("link.exe", "/dynamicbase:no", "a.out", nullptr));
  EXPECT_FALSE(_context.getDynamicBaseEnabled());
}

TEST_F(WinLinkParserTest, Include) {
  EXPECT_FALSE(parse("link.exe", "/include:foo", "a.out", nullptr));
  auto symbols = _context.initialUndefinedSymbols();
  EXPECT_FALSE(symbols.empty());
  EXPECT_EQ("foo", symbols[0]);
  symbols.pop_front();
  EXPECT_TRUE(symbols.empty());
}

TEST_F(WinLinkParserTest, NoInputFiles) {
  EXPECT_TRUE(parse("link.exe", nullptr));
  EXPECT_EQ("No input files\n", errorMessage());
}

TEST_F(WinLinkParserTest, FailIfMismatch_Match) {
  EXPECT_FALSE(parse("link.exe", "/failifmismatch:foo=bar",
                     "/failifmismatch:foo=bar", "/failifmismatch:abc=def",
                     "a.out", nullptr));
}

TEST_F(WinLinkParserTest, FailIfMismatch_Mismatch) {
  EXPECT_TRUE(parse("link.exe", "/failifmismatch:foo=bar",
                    "/failifmismatch:foo=baz", "a.out", nullptr));
}

TEST_F(WinLinkParserTest, Ignore) {
  // There are some no-op command line options that are recognized for
  // compatibility with link.exe.
  EXPECT_FALSE(parse("link.exe", "/nologo", "/errorreport:prompt",
                     "/incremental", "/incremental:no", "a.obj", nullptr));
  EXPECT_EQ("", errorMessage());
  EXPECT_EQ(1, inputFileCount());
  EXPECT_EQ("a.obj", inputFile(0));
}

TEST_F(WinLinkParserTest, DashDash) {
  EXPECT_FALSE(parse("link.exe", "/subsystem:console", "/out:a.exe",
        "a.obj", "--", "b.obj", "-c.obj", nullptr));
  EXPECT_EQ(llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI, _context.getSubsystem());
  EXPECT_EQ("a.exe", _context.outputPath());
  EXPECT_EQ(3, inputFileCount());
  EXPECT_EQ("a.obj", inputFile(0));
  EXPECT_EQ("b.obj", inputFile(1));
  EXPECT_EQ("-c.obj", inputFile(2));
}

TEST_F(WinLinkParserTest, DefEntryNameConsole) {
  EXPECT_FALSE(parse("link.exe", "/subsystem:console", "a.obj", nullptr));
  EXPECT_EQ("_mainCRTStartup", _context.entrySymbolName());
}

TEST_F(WinLinkParserTest, DefEntryNameWindows) {
  EXPECT_FALSE(parse("link.exe", "/subsystem:windows", "a.obj", nullptr));
  EXPECT_EQ("_WinMainCRTStartup", _context.entrySymbolName());
}

} // end anonymous namespace
