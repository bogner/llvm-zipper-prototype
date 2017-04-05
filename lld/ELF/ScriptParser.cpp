//===- ScriptParser.cpp ---------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ScriptParser.h"
#include "Config.h"
#include "Driver.h"
#include "InputSection.h"
#include "LinkerScript.h"
#include "Memory.h"
#include "OutputSections.h"
#include "ScriptLexer.h"
#include "Symbols.h"
#include "Target.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ELF.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <cassert>
#include <limits>
#include <vector>

using namespace llvm;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

static bool isUnderSysroot(StringRef Path);

namespace {
class ScriptParser final : ScriptLexer {
public:
  ScriptParser(MemoryBufferRef MB)
      : ScriptLexer(MB),
        IsUnderSysroot(isUnderSysroot(MB.getBufferIdentifier())) {}

  void readLinkerScript();
  void readVersionScript();
  void readDynamicList();

private:
  void addFile(StringRef Path);

  void readAsNeeded();
  void readEntry();
  void readExtern();
  void readGroup();
  void readInclude();
  void readMemory();
  void readOutput();
  void readOutputArch();
  void readOutputFormat();
  void readPhdrs();
  void readSearchDir();
  void readSections();
  void readVersion();
  void readVersionScriptCommand();

  SymbolAssignment *readAssignment(StringRef Name);
  BytesDataCommand *readBytesDataCommand(StringRef Tok);
  uint32_t readFill();
  OutputSectionCommand *readOutputSectionDescription(StringRef OutSec);
  uint32_t readOutputSectionFiller(StringRef Tok);
  std::vector<StringRef> readOutputSectionPhdrs();
  InputSectionDescription *readInputSectionDescription(StringRef Tok);
  StringMatcher readFilePatterns();
  std::vector<SectionPattern> readInputSectionsList();
  InputSectionDescription *readInputSectionRules(StringRef FilePattern);
  unsigned readPhdrType();
  SortSectionPolicy readSortKind();
  SymbolAssignment *readProvideHidden(bool Provide, bool Hidden);
  SymbolAssignment *readProvideOrAssignment(StringRef Tok);
  void readSort();
  Expr readAssert();

  uint64_t readMemoryAssignment(StringRef, StringRef, StringRef);
  std::pair<uint32_t, uint32_t> readMemoryAttributes();

  Expr readExpr();
  Expr readExpr1(Expr Lhs, int MinPrec);
  StringRef readParenLiteral();
  Expr readPrimary();
  Expr readTernary(Expr Cond);
  Expr readParenExpr();

  // For parsing version script.
  std::vector<SymbolVersion> readVersionExtern();
  void readAnonymousDeclaration();
  void readVersionDeclaration(StringRef VerStr);

  std::pair<std::vector<SymbolVersion>, std::vector<SymbolVersion>>
  readSymbols();

  bool IsUnderSysroot;
};
} // namespace

static bool isUnderSysroot(StringRef Path) {
  if (Config->Sysroot == "")
    return false;
  for (; !Path.empty(); Path = sys::path::parent_path(Path))
    if (sys::fs::equivalent(Config->Sysroot, Path))
      return true;
  return false;
}

// Some operations only support one non absolute value. Move the
// absolute one to the right hand side for convenience.
static void moveAbsRight(ExprValue &A, ExprValue &B) {
  if (A.isAbsolute())
    std::swap(A, B);
  if (!B.isAbsolute())
    error("At least one side of the expression must be absolute");
}

static ExprValue add(ExprValue A, ExprValue B) {
  moveAbsRight(A, B);
  return {A.Sec, A.ForceAbsolute, A.Val + B.getValue()};
}

static ExprValue sub(ExprValue A, ExprValue B) {
  return {A.Sec, A.Val - B.getValue()};
}

static ExprValue mul(ExprValue A, ExprValue B) {
  return A.getValue() * B.getValue();
}

static ExprValue div(ExprValue A, ExprValue B) {
  if (uint64_t BV = B.getValue())
    return A.getValue() / BV;
  error("division by zero");
  return 0;
}

static ExprValue leftShift(ExprValue A, ExprValue B) {
  return A.getValue() << B.getValue();
}

static ExprValue rightShift(ExprValue A, ExprValue B) {
  return A.getValue() >> B.getValue();
}

static ExprValue bitAnd(ExprValue A, ExprValue B) {
  moveAbsRight(A, B);
  return {A.Sec, A.ForceAbsolute,
          (A.getValue() & B.getValue()) - A.getSecAddr()};
}

static ExprValue bitOr(ExprValue A, ExprValue B) {
  moveAbsRight(A, B);
  return {A.Sec, A.ForceAbsolute,
          (A.getValue() | B.getValue()) - A.getSecAddr()};
}

static ExprValue bitNot(ExprValue A) { return ~A.getValue(); }
static ExprValue minus(ExprValue A) { return -A.getValue(); }

void ScriptParser::readDynamicList() {
  expect("{");
  readAnonymousDeclaration();
  if (!atEOF())
    setError("EOF expected, but got " + next());
}

void ScriptParser::readVersionScript() {
  readVersionScriptCommand();
  if (!atEOF())
    setError("EOF expected, but got " + next());
}

void ScriptParser::readVersionScriptCommand() {
  if (consume("{")) {
    readAnonymousDeclaration();
    return;
  }

  while (!atEOF() && !Error && peek() != "}") {
    StringRef VerStr = next();
    if (VerStr == "{") {
      setError("anonymous version definition is used in "
               "combination with other version definitions");
      return;
    }
    expect("{");
    readVersionDeclaration(VerStr);
  }
}

void ScriptParser::readVersion() {
  expect("{");
  readVersionScriptCommand();
  expect("}");
}

void ScriptParser::readLinkerScript() {
  while (!atEOF()) {
    StringRef Tok = next();
    if (Tok == ";")
      continue;

    if (Tok == "ASSERT") {
      Script->Opt.Commands.push_back(make<AssertCommand>(readAssert()));
    } else if (Tok == "ENTRY") {
      readEntry();
    } else if (Tok == "EXTERN") {
      readExtern();
    } else if (Tok == "GROUP" || Tok == "INPUT") {
      readGroup();
    } else if (Tok == "INCLUDE") {
      readInclude();
    } else if (Tok == "MEMORY") {
      readMemory();
    } else if (Tok == "OUTPUT") {
      readOutput();
    } else if (Tok == "OUTPUT_ARCH") {
      readOutputArch();
    } else if (Tok == "OUTPUT_FORMAT") {
      readOutputFormat();
    } else if (Tok == "PHDRS") {
      readPhdrs();
    } else if (Tok == "SEARCH_DIR") {
      readSearchDir();
    } else if (Tok == "SECTIONS") {
      readSections();
    } else if (Tok == "VERSION") {
      readVersion();
    } else if (SymbolAssignment *Cmd = readProvideOrAssignment(Tok)) {
      Script->Opt.Commands.push_back(Cmd);
    } else {
      setError("unknown directive: " + Tok);
    }
  }
}

void ScriptParser::addFile(StringRef S) {
  if (IsUnderSysroot && S.startswith("/")) {
    SmallString<128> PathData;
    StringRef Path = (Config->Sysroot + S).toStringRef(PathData);
    if (sys::fs::exists(Path)) {
      Driver->addFile(Saver.save(Path));
      return;
    }
  }

  if (sys::path::is_absolute(S)) {
    Driver->addFile(S);
  } else if (S.startswith("=")) {
    if (Config->Sysroot.empty())
      Driver->addFile(S.substr(1));
    else
      Driver->addFile(Saver.save(Config->Sysroot + "/" + S.substr(1)));
  } else if (S.startswith("-l")) {
    Driver->addLibrary(S.substr(2));
  } else if (sys::fs::exists(S)) {
    Driver->addFile(S);
  } else {
    if (Optional<std::string> Path = findFromSearchPaths(S))
      Driver->addFile(Saver.save(*Path));
    else
      setError("unable to find " + S);
  }
}

void ScriptParser::readAsNeeded() {
  expect("(");
  bool Orig = Config->AsNeeded;
  Config->AsNeeded = true;
  while (!Error && !consume(")"))
    addFile(unquote(next()));
  Config->AsNeeded = Orig;
}

void ScriptParser::readEntry() {
  // -e <symbol> takes predecence over ENTRY(<symbol>).
  expect("(");
  StringRef Tok = next();
  if (Config->Entry.empty())
    Config->Entry = Tok;
  expect(")");
}

void ScriptParser::readExtern() {
  expect("(");
  while (!Error && !consume(")"))
    Config->Undefined.push_back(next());
}

void ScriptParser::readGroup() {
  expect("(");
  while (!Error && !consume(")")) {
    StringRef Tok = next();
    if (Tok == "AS_NEEDED")
      readAsNeeded();
    else
      addFile(unquote(Tok));
  }
}

void ScriptParser::readInclude() {
  StringRef Tok = unquote(next());

  // https://sourceware.org/binutils/docs/ld/File-Commands.html:
  // The file will be searched for in the current directory, and in any
  // directory specified with the -L option.
  if (sys::fs::exists(Tok)) {
    if (Optional<MemoryBufferRef> MB = readFile(Tok))
      tokenize(*MB);
    return;
  }
  if (Optional<std::string> Path = findFromSearchPaths(Tok)) {
    if (Optional<MemoryBufferRef> MB = readFile(*Path))
      tokenize(*MB);
    return;
  }
  setError("cannot open " + Tok);
}

void ScriptParser::readOutput() {
  // -o <file> takes predecence over OUTPUT(<file>).
  expect("(");
  StringRef Tok = next();
  if (Config->OutputFile.empty())
    Config->OutputFile = unquote(Tok);
  expect(")");
}

void ScriptParser::readOutputArch() {
  // OUTPUT_ARCH is ignored for now.
  expect("(");
  while (!Error && !consume(")"))
    skip();
}

void ScriptParser::readOutputFormat() {
  // Error checking only for now.
  expect("(");
  skip();
  StringRef Tok = next();
  if (Tok == ")")
    return;
  if (Tok != ",") {
    setError("unexpected token: " + Tok);
    return;
  }
  skip();
  expect(",");
  skip();
  expect(")");
}

void ScriptParser::readPhdrs() {
  expect("{");
  while (!Error && !consume("}")) {
    StringRef Tok = next();
    Script->Opt.PhdrsCommands.push_back(
        {Tok, PT_NULL, false, false, UINT_MAX, nullptr});
    PhdrsCommand &PhdrCmd = Script->Opt.PhdrsCommands.back();

    PhdrCmd.Type = readPhdrType();
    do {
      Tok = next();
      if (Tok == ";")
        break;
      if (Tok == "FILEHDR")
        PhdrCmd.HasFilehdr = true;
      else if (Tok == "PHDRS")
        PhdrCmd.HasPhdrs = true;
      else if (Tok == "AT")
        PhdrCmd.LMAExpr = readParenExpr();
      else if (Tok == "FLAGS") {
        expect("(");
        // Passing 0 for the value of dot is a bit of a hack. It means that
        // we accept expressions like ".|1".
        PhdrCmd.Flags = readExpr()().getValue();
        expect(")");
      } else
        setError("unexpected header attribute: " + Tok);
    } while (!Error);
  }
}

void ScriptParser::readSearchDir() {
  expect("(");
  StringRef Tok = next();
  if (!Config->Nostdlib)
    Config->SearchPaths.push_back(unquote(Tok));
  expect(")");
}

void ScriptParser::readSections() {
  Script->Opt.HasSections = true;
  // -no-rosegment is used to avoid placing read only non-executable sections in
  // their own segment. We do the same if SECTIONS command is present in linker
  // script. See comment for computeFlags().
  Config->SingleRoRx = true;

  expect("{");
  while (!Error && !consume("}")) {
    StringRef Tok = next();
    BaseCommand *Cmd = readProvideOrAssignment(Tok);
    if (!Cmd) {
      if (Tok == "ASSERT")
        Cmd = make<AssertCommand>(readAssert());
      else
        Cmd = readOutputSectionDescription(Tok);
    }
    Script->Opt.Commands.push_back(Cmd);
  }
}

static int precedence(StringRef Op) {
  return StringSwitch<int>(Op)
      .Cases("*", "/", 5)
      .Cases("+", "-", 4)
      .Cases("<<", ">>", 3)
      .Cases("<", "<=", ">", ">=", "==", "!=", 2)
      .Cases("&", "|", 1)
      .Default(-1);
}

StringMatcher ScriptParser::readFilePatterns() {
  std::vector<StringRef> V;
  while (!Error && !consume(")"))
    V.push_back(next());
  return StringMatcher(V);
}

SortSectionPolicy ScriptParser::readSortKind() {
  if (consume("SORT") || consume("SORT_BY_NAME"))
    return SortSectionPolicy::Name;
  if (consume("SORT_BY_ALIGNMENT"))
    return SortSectionPolicy::Alignment;
  if (consume("SORT_BY_INIT_PRIORITY"))
    return SortSectionPolicy::Priority;
  if (consume("SORT_NONE"))
    return SortSectionPolicy::None;
  return SortSectionPolicy::Default;
}

// Method reads a list of sequence of excluded files and section globs given in
// a following form: ((EXCLUDE_FILE(file_pattern+))? section_pattern+)+
// Example: *(.foo.1 EXCLUDE_FILE (*a.o) .foo.2 EXCLUDE_FILE (*b.o) .foo.3)
// The semantics of that is next:
// * Include .foo.1 from every file.
// * Include .foo.2 from every file but a.o
// * Include .foo.3 from every file but b.o
std::vector<SectionPattern> ScriptParser::readInputSectionsList() {
  std::vector<SectionPattern> Ret;
  while (!Error && peek() != ")") {
    StringMatcher ExcludeFilePat;
    if (consume("EXCLUDE_FILE")) {
      expect("(");
      ExcludeFilePat = readFilePatterns();
    }

    std::vector<StringRef> V;
    while (!Error && peek() != ")" && peek() != "EXCLUDE_FILE")
      V.push_back(next());

    if (!V.empty())
      Ret.push_back({std::move(ExcludeFilePat), StringMatcher(V)});
    else
      setError("section pattern is expected");
  }
  return Ret;
}

// Reads contents of "SECTIONS" directive. That directive contains a
// list of glob patterns for input sections. The grammar is as follows.
//
// <patterns> ::= <section-list>
//              | <sort> "(" <section-list> ")"
//              | <sort> "(" <sort> "(" <section-list> ")" ")"
//
// <sort>     ::= "SORT" | "SORT_BY_NAME" | "SORT_BY_ALIGNMENT"
//              | "SORT_BY_INIT_PRIORITY" | "SORT_NONE"
//
// <section-list> is parsed by readInputSectionsList().
InputSectionDescription *
ScriptParser::readInputSectionRules(StringRef FilePattern) {
  auto *Cmd = make<InputSectionDescription>(FilePattern);
  expect("(");

  while (!Error && !consume(")")) {
    SortSectionPolicy Outer = readSortKind();
    SortSectionPolicy Inner = SortSectionPolicy::Default;
    std::vector<SectionPattern> V;
    if (Outer != SortSectionPolicy::Default) {
      expect("(");
      Inner = readSortKind();
      if (Inner != SortSectionPolicy::Default) {
        expect("(");
        V = readInputSectionsList();
        expect(")");
      } else {
        V = readInputSectionsList();
      }
      expect(")");
    } else {
      V = readInputSectionsList();
    }

    for (SectionPattern &Pat : V) {
      Pat.SortInner = Inner;
      Pat.SortOuter = Outer;
    }

    std::move(V.begin(), V.end(), std::back_inserter(Cmd->SectionPatterns));
  }
  return Cmd;
}

InputSectionDescription *
ScriptParser::readInputSectionDescription(StringRef Tok) {
  // Input section wildcard can be surrounded by KEEP.
  // https://sourceware.org/binutils/docs/ld/Input-Section-Keep.html#Input-Section-Keep
  if (Tok == "KEEP") {
    expect("(");
    StringRef FilePattern = next();
    InputSectionDescription *Cmd = readInputSectionRules(FilePattern);
    expect(")");
    Script->Opt.KeptSections.push_back(Cmd);
    return Cmd;
  }
  return readInputSectionRules(Tok);
}

void ScriptParser::readSort() {
  expect("(");
  expect("CONSTRUCTORS");
  expect(")");
}

Expr ScriptParser::readAssert() {
  expect("(");
  Expr E = readExpr();
  expect(",");
  StringRef Msg = unquote(next());
  expect(")");
  return [=] {
    if (!E().getValue())
      error(Msg);
    return Script->getDot();
  };
}

// Reads a FILL(expr) command. We handle the FILL command as an
// alias for =fillexp section attribute, which is different from
// what GNU linkers do.
// https://sourceware.org/binutils/docs/ld/Output-Section-Data.html
uint32_t ScriptParser::readFill() {
  expect("(");
  uint32_t V = readOutputSectionFiller(next());
  expect(")");
  expect(";");
  return V;
}

OutputSectionCommand *
ScriptParser::readOutputSectionDescription(StringRef OutSec) {
  OutputSectionCommand *Cmd = make<OutputSectionCommand>(OutSec);
  Cmd->Location = getCurrentLocation();

  // Read an address expression.
  // https://sourceware.org/binutils/docs/ld/Output-Section-Address.html#Output-Section-Address
  if (peek() != ":")
    Cmd->AddrExpr = readExpr();

  expect(":");

  if (consume("AT"))
    Cmd->LMAExpr = readParenExpr();
  if (consume("ALIGN"))
    Cmd->AlignExpr = readParenExpr();
  if (consume("SUBALIGN"))
    Cmd->SubalignExpr = readParenExpr();

  // Parse constraints.
  if (consume("ONLY_IF_RO"))
    Cmd->Constraint = ConstraintKind::ReadOnly;
  if (consume("ONLY_IF_RW"))
    Cmd->Constraint = ConstraintKind::ReadWrite;
  expect("{");

  while (!Error && !consume("}")) {
    StringRef Tok = next();
    if (Tok == ";") {
      // Empty commands are allowed. Do nothing here.
    } else if (SymbolAssignment *Assignment = readProvideOrAssignment(Tok)) {
      Cmd->Commands.push_back(Assignment);
    } else if (BytesDataCommand *Data = readBytesDataCommand(Tok)) {
      Cmd->Commands.push_back(Data);
    } else if (Tok == "ASSERT") {
      Cmd->Commands.push_back(make<AssertCommand>(readAssert()));
      expect(";");
    } else if (Tok == "CONSTRUCTORS") {
      // CONSTRUCTORS is a keyword to make the linker recognize C++ ctors/dtors
      // by name. This is for very old file formats such as ECOFF/XCOFF.
      // For ELF, we should ignore.
    } else if (Tok == "FILL") {
      Cmd->Filler = readFill();
    } else if (Tok == "SORT") {
      readSort();
    } else if (peek() == "(") {
      Cmd->Commands.push_back(readInputSectionDescription(Tok));
    } else {
      setError("unknown command " + Tok);
    }
  }

  if (consume(">"))
    Cmd->MemoryRegionName = next();

  Cmd->Phdrs = readOutputSectionPhdrs();

  if (consume("="))
    Cmd->Filler = readOutputSectionFiller(next());
  else if (peek().startswith("="))
    Cmd->Filler = readOutputSectionFiller(next().drop_front());

  // Consume optional comma following output section command.
  consume(",");

  return Cmd;
}

// Read "=<number>" where <number> is an octal/decimal/hexadecimal number.
// https://sourceware.org/binutils/docs/ld/Output-Section-Fill.html
//
// ld.gold is not fully compatible with ld.bfd. ld.bfd handles
// hexstrings as blobs of arbitrary sizes, while ld.gold handles them
// as 32-bit big-endian values. We will do the same as ld.gold does
// because it's simpler than what ld.bfd does.
uint32_t ScriptParser::readOutputSectionFiller(StringRef Tok) {
  uint32_t V;
  if (!Tok.getAsInteger(0, V))
    return V;
  setError("invalid filler expression: " + Tok);
  return 0;
}

SymbolAssignment *ScriptParser::readProvideHidden(bool Provide, bool Hidden) {
  expect("(");
  SymbolAssignment *Cmd = readAssignment(next());
  Cmd->Provide = Provide;
  Cmd->Hidden = Hidden;
  expect(")");
  expect(";");
  return Cmd;
}

SymbolAssignment *ScriptParser::readProvideOrAssignment(StringRef Tok) {
  SymbolAssignment *Cmd = nullptr;
  if (peek() == "=" || peek() == "+=") {
    Cmd = readAssignment(Tok);
    expect(";");
  } else if (Tok == "PROVIDE") {
    Cmd = readProvideHidden(true, false);
  } else if (Tok == "HIDDEN") {
    Cmd = readProvideHidden(false, true);
  } else if (Tok == "PROVIDE_HIDDEN") {
    Cmd = readProvideHidden(true, true);
  }
  return Cmd;
}

SymbolAssignment *ScriptParser::readAssignment(StringRef Name) {
  StringRef Op = next();
  assert(Op == "=" || Op == "+=");
  Expr E = readExpr();
  if (Op == "+=") {
    std::string Loc = getCurrentLocation();
    E = [=] { return add(Script->getSymbolValue(Loc, Name), E()); };
  }
  return make<SymbolAssignment>(Name, E, getCurrentLocation());
}

// This is an operator-precedence parser to parse a linker
// script expression.
Expr ScriptParser::readExpr() {
  // Our lexer is context-aware. Set the in-expression bit so that
  // they apply different tokenization rules.
  bool Orig = InExpr;
  InExpr = true;
  Expr E = readExpr1(readPrimary(), 0);
  InExpr = Orig;
  return E;
}

static Expr combine(StringRef Op, Expr L, Expr R) {
  if (Op == "*")
    return [=] { return mul(L(), R()); };
  if (Op == "/") {
    return [=] { return div(L(), R()); };
  }
  if (Op == "+")
    return [=] { return add(L(), R()); };
  if (Op == "-")
    return [=] { return sub(L(), R()); };
  if (Op == "<<")
    return [=] { return leftShift(L(), R()); };
  if (Op == ">>")
    return [=] { return rightShift(L(), R()); };
  if (Op == "<")
    return [=] { return L().getValue() < R().getValue(); };
  if (Op == ">")
    return [=] { return L().getValue() > R().getValue(); };
  if (Op == ">=")
    return [=] { return L().getValue() >= R().getValue(); };
  if (Op == "<=")
    return [=] { return L().getValue() <= R().getValue(); };
  if (Op == "==")
    return [=] { return L().getValue() == R().getValue(); };
  if (Op == "!=")
    return [=] { return L().getValue() != R().getValue(); };
  if (Op == "&")
    return [=] { return bitAnd(L(), R()); };
  if (Op == "|")
    return [=] { return bitOr(L(), R()); };
  llvm_unreachable("invalid operator");
}

// This is a part of the operator-precedence parser. This function
// assumes that the remaining token stream starts with an operator.
Expr ScriptParser::readExpr1(Expr Lhs, int MinPrec) {
  while (!atEOF() && !Error) {
    // Read an operator and an expression.
    if (consume("?"))
      return readTernary(Lhs);
    StringRef Op1 = peek();
    if (precedence(Op1) < MinPrec)
      break;
    skip();
    Expr Rhs = readPrimary();

    // Evaluate the remaining part of the expression first if the
    // next operator has greater precedence than the previous one.
    // For example, if we have read "+" and "3", and if the next
    // operator is "*", then we'll evaluate 3 * ... part first.
    while (!atEOF()) {
      StringRef Op2 = peek();
      if (precedence(Op2) <= precedence(Op1))
        break;
      Rhs = readExpr1(Rhs, precedence(Op2));
    }

    Lhs = combine(Op1, Lhs, Rhs);
  }
  return Lhs;
}

uint64_t static getConstant(StringRef S) {
  if (S == "COMMONPAGESIZE")
    return Target->PageSize;
  if (S == "MAXPAGESIZE")
    return Config->MaxPageSize;
  error("unknown constant: " + S);
  return 0;
}

// Parses Tok as an integer. Returns true if successful.
// It recognizes hexadecimal (prefixed with "0x" or suffixed with "H")
// and decimal numbers. Decimal numbers may have "K" (kilo) or
// "M" (mega) prefixes.
static bool readInteger(StringRef Tok, uint64_t &Result) {
  // Negative number
  if (Tok.startswith("-")) {
    if (!readInteger(Tok.substr(1), Result))
      return false;
    Result = -Result;
    return true;
  }

  // Hexadecimal
  if (Tok.startswith_lower("0x"))
    return !Tok.substr(2).getAsInteger(16, Result);
  if (Tok.endswith_lower("H"))
    return !Tok.drop_back().getAsInteger(16, Result);

  // Decimal
  int Suffix = 1;
  if (Tok.endswith_lower("K")) {
    Suffix = 1024;
    Tok = Tok.drop_back();
  } else if (Tok.endswith_lower("M")) {
    Suffix = 1024 * 1024;
    Tok = Tok.drop_back();
  }
  if (Tok.getAsInteger(10, Result))
    return false;
  Result *= Suffix;
  return true;
}

BytesDataCommand *ScriptParser::readBytesDataCommand(StringRef Tok) {
  int Size = StringSwitch<unsigned>(Tok)
                 .Case("BYTE", 1)
                 .Case("SHORT", 2)
                 .Case("LONG", 4)
                 .Case("QUAD", 8)
                 .Default(-1);
  if (Size == -1)
    return nullptr;

  return make<BytesDataCommand>(readParenExpr(), Size);
}

StringRef ScriptParser::readParenLiteral() {
  expect("(");
  StringRef Tok = next();
  expect(")");
  return Tok;
}

Expr ScriptParser::readPrimary() {
  if (peek() == "(")
    return readParenExpr();

  StringRef Tok = next();
  std::string Location = getCurrentLocation();

  if (Tok == "~") {
    Expr E = readPrimary();
    return [=] { return bitNot(E()); };
  }
  if (Tok == "-") {
    Expr E = readPrimary();
    return [=] { return minus(E()); };
  }

  // Built-in functions are parsed here.
  // https://sourceware.org/binutils/docs/ld/Builtin-Functions.html.
  if (Tok == "ABSOLUTE") {
    Expr Inner = readParenExpr();
    return [=] {
      ExprValue I = Inner();
      I.ForceAbsolute = true;
      return I;
    };
  }
  if (Tok == "ADDR") {
    StringRef Name = readParenLiteral();
    return [=]() -> ExprValue {
      return {Script->getOutputSection(Location, Name), 0};
    };
  }
  if (Tok == "ALIGN") {
    expect("(");
    Expr E = readExpr();
    if (consume(",")) {
      Expr E2 = readExpr();
      expect(")");
      return [=] { return alignTo(E().getValue(), E2().getValue()); };
    }
    expect(")");
    return [=] { return alignTo(Script->getDot(), E().getValue()); };
  }
  if (Tok == "ALIGNOF") {
    StringRef Name = readParenLiteral();
    return [=] { return Script->getOutputSection(Location, Name)->Alignment; };
  }
  if (Tok == "ASSERT")
    return readAssert();
  if (Tok == "CONSTANT") {
    StringRef Name = readParenLiteral();
    return [=] { return getConstant(Name); };
  }
  if (Tok == "DATA_SEGMENT_ALIGN") {
    expect("(");
    Expr E = readExpr();
    expect(",");
    readExpr();
    expect(")");
    return [=] { return alignTo(Script->getDot(), E().getValue()); };
  }
  if (Tok == "DATA_SEGMENT_END") {
    expect("(");
    expect(".");
    expect(")");
    return [] { return Script->getDot(); };
  }
  if (Tok == "DATA_SEGMENT_RELRO_END") {
    // GNU linkers implements more complicated logic to handle
    // DATA_SEGMENT_RELRO_END. We instead ignore the arguments and
    // just align to the next page boundary for simplicity.
    expect("(");
    readExpr();
    expect(",");
    readExpr();
    expect(")");
    return [] { return alignTo(Script->getDot(), Target->PageSize); };
  }
  if (Tok == "DEFINED") {
    StringRef Name = readParenLiteral();
    return [=] { return Script->isDefined(Name) ? 1 : 0; };
  }
  if (Tok == "LOADADDR") {
    StringRef Name = readParenLiteral();
    return [=] { return Script->getOutputSection(Location, Name)->getLMA(); };
  }
  if (Tok == "SEGMENT_START") {
    expect("(");
    skip();
    expect(",");
    Expr E = readExpr();
    expect(")");
    return [=] { return E(); };
  }
  if (Tok == "SIZEOF") {
    StringRef Name = readParenLiteral();
    return [=] { return Script->getOutputSectionSize(Name); };
  }
  if (Tok == "SIZEOF_HEADERS")
    return [=] { return elf::getHeaderSize(); };

  // Tok is a literal number.
  uint64_t V;
  if (readInteger(Tok, V))
    return [=] { return V; };

  // Tok is a symbol name.
  if (Tok != ".") {
    if (!isValidCIdentifier(Tok))
      setError("malformed number: " + Tok);
    Script->Opt.UndefinedSymbols.push_back(Tok);
  }
  return [=] { return Script->getSymbolValue(Location, Tok); };
}

Expr ScriptParser::readTernary(Expr Cond) {
  Expr L = readExpr();
  expect(":");
  Expr R = readExpr();
  return [=] { return Cond().getValue() ? L() : R(); };
}

Expr ScriptParser::readParenExpr() {
  expect("(");
  Expr E = readExpr();
  expect(")");
  return E;
}

std::vector<StringRef> ScriptParser::readOutputSectionPhdrs() {
  std::vector<StringRef> Phdrs;
  while (!Error && peek().startswith(":")) {
    StringRef Tok = next();
    Phdrs.push_back((Tok.size() == 1) ? next() : Tok.substr(1));
  }
  return Phdrs;
}

// Read a program header type name. The next token must be a
// name of a program header type or a constant (e.g. "0x3").
unsigned ScriptParser::readPhdrType() {
  StringRef Tok = next();
  uint64_t Val;
  if (readInteger(Tok, Val))
    return Val;

  unsigned Ret = StringSwitch<unsigned>(Tok)
                     .Case("PT_NULL", PT_NULL)
                     .Case("PT_LOAD", PT_LOAD)
                     .Case("PT_DYNAMIC", PT_DYNAMIC)
                     .Case("PT_INTERP", PT_INTERP)
                     .Case("PT_NOTE", PT_NOTE)
                     .Case("PT_SHLIB", PT_SHLIB)
                     .Case("PT_PHDR", PT_PHDR)
                     .Case("PT_TLS", PT_TLS)
                     .Case("PT_GNU_EH_FRAME", PT_GNU_EH_FRAME)
                     .Case("PT_GNU_STACK", PT_GNU_STACK)
                     .Case("PT_GNU_RELRO", PT_GNU_RELRO)
                     .Case("PT_OPENBSD_RANDOMIZE", PT_OPENBSD_RANDOMIZE)
                     .Case("PT_OPENBSD_WXNEEDED", PT_OPENBSD_WXNEEDED)
                     .Case("PT_OPENBSD_BOOTDATA", PT_OPENBSD_BOOTDATA)
                     .Default(-1);

  if (Ret == (unsigned)-1) {
    setError("invalid program header type: " + Tok);
    return PT_NULL;
  }
  return Ret;
}

// Reads an anonymous version declaration.
void ScriptParser::readAnonymousDeclaration() {
  std::vector<SymbolVersion> Locals;
  std::vector<SymbolVersion> Globals;
  std::tie(Locals, Globals) = readSymbols();

  for (SymbolVersion V : Locals) {
    if (V.Name == "*")
      Config->DefaultSymbolVersion = VER_NDX_LOCAL;
    else
      Config->VersionScriptLocals.push_back(V);
  }

  for (SymbolVersion V : Globals)
    Config->VersionScriptGlobals.push_back(V);

  expect(";");
}

// Reads a non-anonymous version definition,
// e.g. "VerStr { global: foo; bar; local: *; };".
void ScriptParser::readVersionDeclaration(StringRef VerStr) {
  // Read a symbol list.
  std::vector<SymbolVersion> Locals;
  std::vector<SymbolVersion> Globals;
  std::tie(Locals, Globals) = readSymbols();

  for (SymbolVersion V : Locals) {
    if (V.Name == "*")
      Config->DefaultSymbolVersion = VER_NDX_LOCAL;
    else
      Config->VersionScriptLocals.push_back(V);
  }

  // Create a new version definition and add that to the global symbols.
  VersionDefinition Ver;
  Ver.Name = VerStr;
  Ver.Globals = Globals;

  // User-defined version number starts from 2 because 0 and 1 are
  // reserved for VER_NDX_LOCAL and VER_NDX_GLOBAL, respectively.
  Ver.Id = Config->VersionDefinitions.size() + 2;
  Config->VersionDefinitions.push_back(Ver);

  // Each version may have a parent version. For example, "Ver2"
  // defined as "Ver2 { global: foo; local: *; } Ver1;" has "Ver1"
  // as a parent. This version hierarchy is, probably against your
  // instinct, purely for hint; the runtime doesn't care about it
  // at all. In LLD, we simply ignore it.
  if (peek() != ";")
    skip();
  expect(";");
}

// Reads a list of symbols, e.g. "{ global: foo; bar; local: *; };".
std::pair<std::vector<SymbolVersion>, std::vector<SymbolVersion>>
ScriptParser::readSymbols() {
  std::vector<SymbolVersion> Locals;
  std::vector<SymbolVersion> Globals;
  std::vector<SymbolVersion> *V = &Globals;

  while (!Error) {
    if (consume("}"))
      break;
    if (consumeLabel("local")) {
      V = &Locals;
      continue;
    }
    if (consumeLabel("global")) {
      V = &Globals;
      continue;
    }

    if (consume("extern")) {
      std::vector<SymbolVersion> Ext = readVersionExtern();
      V->insert(V->end(), Ext.begin(), Ext.end());
    } else {
      StringRef Tok = next();
      V->push_back({unquote(Tok), false, hasWildcard(Tok)});
    }
    expect(";");
  }
  return {Locals, Globals};
}

// Reads an "extern C++" directive, e.g.,
// "extern "C++" { ns::*; "f(int, double)"; };"
std::vector<SymbolVersion> ScriptParser::readVersionExtern() {
  StringRef Tok = next();
  bool IsCXX = Tok == "\"C++\"";
  if (!IsCXX && Tok != "\"C\"")
    setError("Unknown language");
  expect("{");

  std::vector<SymbolVersion> Ret;
  while (!Error && peek() != "}") {
    StringRef Tok = next();
    bool HasWildcard = !Tok.startswith("\"") && hasWildcard(Tok);
    Ret.push_back({unquote(Tok), IsCXX, HasWildcard});
    expect(";");
  }

  expect("}");
  return Ret;
}

uint64_t ScriptParser::readMemoryAssignment(StringRef S1, StringRef S2,
                                            StringRef S3) {
  if (!(consume(S1) || consume(S2) || consume(S3))) {
    setError("expected one of: " + S1 + ", " + S2 + ", or " + S3);
    return 0;
  }
  expect("=");

  // TODO: Fully support constant expressions.
  uint64_t Val;
  if (!readInteger(next(), Val))
    setError("nonconstant expression for " + S1);
  return Val;
}

// Parse the MEMORY command as specified in:
// https://sourceware.org/binutils/docs/ld/MEMORY.html
//
// MEMORY { name [(attr)] : ORIGIN = origin, LENGTH = len ... }
void ScriptParser::readMemory() {
  expect("{");
  while (!Error && !consume("}")) {
    StringRef Name = next();

    uint32_t Flags = 0;
    uint32_t NegFlags = 0;
    if (consume("(")) {
      std::tie(Flags, NegFlags) = readMemoryAttributes();
      expect(")");
    }
    expect(":");

    uint64_t Origin = readMemoryAssignment("ORIGIN", "org", "o");
    expect(",");
    uint64_t Length = readMemoryAssignment("LENGTH", "len", "l");

    // Add the memory region to the region map (if it doesn't already exist).
    auto It = Script->Opt.MemoryRegions.find(Name);
    if (It != Script->Opt.MemoryRegions.end())
      setError("region '" + Name + "' already defined");
    else
      Script->Opt.MemoryRegions[Name] = {Name,   Origin, Length,
                                         Origin, Flags,  NegFlags};
  }
}

// This function parses the attributes used to match against section
// flags when placing output sections in a memory region. These flags
// are only used when an explicit memory region name is not used.
std::pair<uint32_t, uint32_t> ScriptParser::readMemoryAttributes() {
  uint32_t Flags = 0;
  uint32_t NegFlags = 0;
  bool Invert = false;

  for (char C : next().lower()) {
    uint32_t Flag = 0;
    if (C == '!')
      Invert = !Invert;
    else if (C == 'w')
      Flag = SHF_WRITE;
    else if (C == 'x')
      Flag = SHF_EXECINSTR;
    else if (C == 'a')
      Flag = SHF_ALLOC;
    else if (C != 'r')
      setError("invalid memory region attribute");

    if (Invert)
      NegFlags |= Flag;
    else
      Flags |= Flag;
  }
  return {Flags, NegFlags};
}

void elf::readLinkerScript(MemoryBufferRef MB) {
  ScriptParser(MB).readLinkerScript();
}

void elf::readVersionScript(MemoryBufferRef MB) {
  ScriptParser(MB).readVersionScript();
}

void elf::readDynamicList(MemoryBufferRef MB) {
  ScriptParser(MB).readDynamicList();
}
