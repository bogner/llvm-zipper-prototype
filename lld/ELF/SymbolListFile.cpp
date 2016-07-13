//===- SymbolListFile.cpp -------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the parser/evaluator of the linker script.
// It does not construct an AST but consume linker script directives directly.
// Results are written to Driver or Config object.
//
//===----------------------------------------------------------------------===//

#include "SymbolListFile.h"
#include "Config.h"
#include "ScriptParser.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace lld;
using namespace lld::elf;

// Parse the --dynamic-list argument.  A dynamic list is in the form
//
//  { symbol1; symbol2; [...]; symbolN };
//
// Multiple groups can be defined in the same file and they are merged
// in only one definition.

class DynamicListParser final : public ScriptParserBase {
public:
  DynamicListParser(StringRef S) : ScriptParserBase(S) {}

  void run();

private:
  void readGroup();
};

// Parse the default group definition using C language symbol name.
void DynamicListParser::readGroup() {
  expect("{");
  while (!Error) {
    Config->DynamicList.push_back(next());
    expect(";");
    if (peek() == "}") {
      next();
      break;
    }
  }
  expect(";");
}

void DynamicListParser::run() {
  while (!atEOF())
    readGroup();
}

void elf::parseDynamicList(MemoryBufferRef MB) {
  DynamicListParser(MB.getBuffer()).run();
}

// Parse the --version-script argument. We currently only accept the following
// version script syntax:
//
//  { [ global: symbol1; symbol2; [...]; symbolN; ] local: *; };
//
// No wildcards are supported, other than for the local entry. Symbol versioning
// is also not supported.

class VersionScriptParser final : public ScriptParserBase {
public:
  VersionScriptParser(StringRef S) : ScriptParserBase(S) {}

  void run();

private:
  void parseVersion(StringRef Version);
  void parseLocal();
  void parseVersionSymbols(StringRef Version);
};

size_t elf::defineSymbolVersion(StringRef Version) {
  // Identifiers start at 2 because 0 and 1 are reserved
  // for VER_NDX_LOCAL and VER_NDX_GLOBAL constants.
  size_t VersionId = Config->SymbolVersions.size() + 2;
  Config->SymbolVersions.push_back(elf::Version(Version, VersionId));
  return VersionId;
}

void VersionScriptParser::parseVersion(StringRef Version) {
  expect("{");
  defineSymbolVersion(Version);
  if (peek() == "global:") {
    next();
    parseVersionSymbols(Version);
  }
  if (peek() == "local:")
    parseLocal();
  else if (peek() != "}")
    parseVersionSymbols(Version);

  expect("}");

  // Each version may have a parent version. For example, "Ver2" defined as
  // "Ver2 { global: foo; local: *; } Ver1;" has "Ver1" as a parent. This
  // version hierarchy is, probably against your instinct, purely for human; the
  // runtime doesn't care about them at all. In LLD, we simply skip the token.
  if (!Version.empty() && peek() != ";")
    next();
  expect(";");
}

void VersionScriptParser::parseLocal() {
  expect("local:");
  expect("*");
  expect(";");
  Config->VersionScriptGlobalByDefault = false;
}

void VersionScriptParser::parseVersionSymbols(StringRef Version) {
  std::vector<StringRef> *Globals;
  if (Version.empty())
    Globals = &Config->VersionScriptGlobals;
  else
    Globals = &Config->SymbolVersions.back().Globals;

  for (;;) {
    StringRef Cur = peek();
    if (Cur == "extern")
      setError("extern keyword is not supported");
    if (Cur == "}" || Cur == "local:" || Error)
      return;
    next();
    Globals->push_back(Cur);
    expect(";");
  }
}

void VersionScriptParser::run() {
  StringRef Msg = "anonymous version definition is used in "
                  "combination with other version definitions";
  if (peek() == "{") {
    parseVersion("");
    if (!atEOF())
      setError(Msg);
    return;
  }

  while (!atEOF() && !Error) {
    if (peek() == "{") {
      setError(Msg);
      return;
    }
    parseVersion(next());
  }
}

void elf::parseVersionScript(MemoryBufferRef MB) {
  VersionScriptParser(MB.getBuffer()).run();
}
