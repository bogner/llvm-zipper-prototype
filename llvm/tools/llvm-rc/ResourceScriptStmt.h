//===-- ResourceScriptStmt.h ------------------------------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
//
// This lists all the resource and statement types occurring in RC scripts.
//
//===---------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVMRC_RESOURCESCRIPTSTMT_H
#define LLVM_TOOLS_LLVMRC_RESOURCESCRIPTSTMT_H

#include "ResourceScriptToken.h"

namespace llvm {
namespace rc {

// A class holding a name - either an integer or a reference to the string.
class IntOrString {
private:
  union Data {
    uint32_t Int;
    StringRef String;
    Data(uint32_t Value) : Int(Value) {}
    Data(const StringRef Value) : String(Value) {}
    Data(const RCToken &Token) {
      if (Token.kind() == RCToken::Kind::Int)
        Int = Token.intValue();
      else
        String = Token.value();
    }
  } Data;
  bool IsInt;

public:
  IntOrString() : IntOrString(0) {}
  IntOrString(uint32_t Value) : Data(Value), IsInt(1) {}
  IntOrString(StringRef Value) : Data(Value), IsInt(0) {}
  IntOrString(const RCToken &Token)
      : Data(Token), IsInt(Token.kind() == RCToken::Kind::Int) {}

  bool equalsLower(const char *Str) {
    return !IsInt && Data.String.equals_lower(Str);
  }

  friend raw_ostream &operator<<(raw_ostream &, const IntOrString &);
};

// Base resource. All the resources should derive from this base.
class RCResource {
protected:
  IntOrString ResName;

public:
  RCResource() = default;
  RCResource(RCResource &&) = default;
  void setName(const IntOrString &Name) { ResName = Name; }
  virtual raw_ostream &log(raw_ostream &OS) const {
    return OS << "Base statement\n";
  };
  virtual ~RCResource() {}
};

// Optional statement base. All such statements should derive from this base.
class OptionalStmt : public RCResource {};

class OptionalStmtList : public OptionalStmt {
  std::vector<std::unique_ptr<OptionalStmt>> Statements;

public:
  OptionalStmtList() {}
  virtual raw_ostream &log(raw_ostream &OS) const;

  void addStmt(std::unique_ptr<OptionalStmt> Stmt) {
    Statements.push_back(std::move(Stmt));
  }
};

// LANGUAGE statement. It can occur both as a top-level statement (in such
// a situation, it changes the default language until the end of the file)
// and as an optional resource statement (then it changes the language
// of a single resource).
//
// Ref: msdn.microsoft.com/en-us/library/windows/desktop/aa381019(v=vs.85).aspx
class LanguageResource : public OptionalStmt {
  uint32_t Lang, SubLang;

public:
  LanguageResource(uint32_t LangId, uint32_t SubLangId)
      : Lang(LangId), SubLang(SubLangId) {}
  raw_ostream &log(raw_ostream &) const override;
};

// ACCELERATORS resource. Defines a named table of accelerators for the app.
//
// Ref: msdn.microsoft.com/en-us/library/windows/desktop/aa380610(v=vs.85).aspx
class AcceleratorsResource : public RCResource {
public:
  class Accelerator {
  public:
    IntOrString Event;
    uint32_t Id;
    uint8_t Flags;

    enum Options {
      ASCII = (1 << 0),
      VIRTKEY = (1 << 1),
      NOINVERT = (1 << 2),
      ALT = (1 << 3),
      SHIFT = (1 << 4),
      CONTROL = (1 << 5)
    };

    static constexpr size_t NumFlags = 6;
    static StringRef OptionsStr[NumFlags];
  };

  AcceleratorsResource(OptionalStmtList &&OptStmts)
      : OptStatements(std::move(OptStmts)) {}
  void addAccelerator(IntOrString Event, uint32_t Id, uint8_t Flags) {
    Accelerators.push_back(Accelerator{Event, Id, Flags});
  }
  raw_ostream &log(raw_ostream &) const override;

private:
  std::vector<Accelerator> Accelerators;
  OptionalStmtList OptStatements;
};

// CURSOR resource. Represents a single cursor (".cur") file.
//
// Ref: msdn.microsoft.com/en-us/library/windows/desktop/aa380920(v=vs.85).aspx
class CursorResource : public RCResource {
  StringRef CursorLoc;

public:
  CursorResource(StringRef Location) : CursorLoc(Location) {}
  raw_ostream &log(raw_ostream &) const override;
};

// ICON resource. Represents a single ".ico" file containing a group of icons.
//
// Ref: msdn.microsoft.com/en-us/library/windows/desktop/aa381018(v=vs.85).aspx
class IconResource : public RCResource {
  StringRef IconLoc;

public:
  IconResource(StringRef Location) : IconLoc(Location) {}
  raw_ostream &log(raw_ostream &) const override;
};

// HTML resource. Represents a local webpage that is to be embedded into the
// resulting resource file. It embeds a file only - no additional resources
// (images etc.) are included with this resource.
//
// Ref: msdn.microsoft.com/en-us/library/windows/desktop/aa966018(v=vs.85).aspx
class HTMLResource : public RCResource {
  StringRef HTMLLoc;

public:
  HTMLResource(StringRef Location) : HTMLLoc(Location) {}
  raw_ostream &log(raw_ostream &) const override;
};

// STRINGTABLE resource. Contains a list of strings, each having its unique ID.
//
// Ref: msdn.microsoft.com/en-us/library/windows/desktop/aa381050(v=vs.85).aspx
class StringTableResource : public RCResource {
  OptionalStmtList OptStatements;
  std::vector<std::pair<uint32_t, StringRef>> Table;

public:
  StringTableResource(OptionalStmtList &&OptStmts)
      : OptStatements(std::move(OptStmts)) {}
  void addString(uint32_t ID, StringRef String) {
    Table.emplace_back(ID, String);
  }
  raw_ostream &log(raw_ostream &) const override;
};

// CHARACTERISTICS optional statement.
//
// Ref: msdn.microsoft.com/en-us/library/windows/desktop/aa380872(v=vs.85).aspx
class CharacteristicsStmt : public OptionalStmt {
  uint32_t Value;

public:
  CharacteristicsStmt(uint32_t Characteristic) : Value(Characteristic) {}
  raw_ostream &log(raw_ostream &) const override;
};

// VERSION optional statement.
//
// Ref: msdn.microsoft.com/en-us/library/windows/desktop/aa381059(v=vs.85).aspx
class VersionStmt : public OptionalStmt {
  uint32_t Value;

public:
  VersionStmt(uint32_t Version) : Value(Version) {}
  raw_ostream &log(raw_ostream &) const override;
};

} // namespace rc
} // namespace llvm

#endif
