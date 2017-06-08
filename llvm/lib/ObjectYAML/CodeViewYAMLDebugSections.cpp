//===- CodeViewYAMLDebugSections.cpp - CodeView YAMLIO debug sections -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines classes for handling the YAML representation of CodeView
// Debug Info.
//
//===----------------------------------------------------------------------===//

#include "llvm/ObjectYAML/CodeViewYAMLDebugSections.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/DebugInfo/CodeView/CodeViewError.h"
#include "llvm/DebugInfo/CodeView/DebugChecksumsSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugCrossExSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugCrossImpSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugInlineeLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugStringTableSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugSubsectionVisitor.h"
#include "llvm/DebugInfo/CodeView/EnumTables.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::CodeViewYAML;
using namespace llvm::CodeViewYAML::detail;
using namespace llvm::yaml;

LLVM_YAML_IS_SEQUENCE_VECTOR(SourceFileChecksumEntry)
LLVM_YAML_IS_SEQUENCE_VECTOR(SourceLineEntry)
LLVM_YAML_IS_SEQUENCE_VECTOR(SourceColumnEntry)
LLVM_YAML_IS_SEQUENCE_VECTOR(SourceLineBlock)
LLVM_YAML_IS_SEQUENCE_VECTOR(SourceLineInfo)
LLVM_YAML_IS_SEQUENCE_VECTOR(InlineeSite)
LLVM_YAML_IS_SEQUENCE_VECTOR(InlineeInfo)
LLVM_YAML_IS_SEQUENCE_VECTOR(CrossModuleExport)
LLVM_YAML_IS_SEQUENCE_VECTOR(YAMLCrossModuleImport)
LLVM_YAML_IS_SEQUENCE_VECTOR(StringRef)
LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(uint32_t)

LLVM_YAML_DECLARE_SCALAR_TRAITS(HexFormattedString, false)
LLVM_YAML_DECLARE_ENUM_TRAITS(DebugSubsectionKind)
LLVM_YAML_DECLARE_ENUM_TRAITS(FileChecksumKind)
LLVM_YAML_DECLARE_BITSET_TRAITS(LineFlags)

LLVM_YAML_DECLARE_MAPPING_TRAITS(CrossModuleExport)
LLVM_YAML_DECLARE_MAPPING_TRAITS(YAMLCrossModuleImport)
LLVM_YAML_DECLARE_MAPPING_TRAITS(CrossModuleImportItem)
LLVM_YAML_DECLARE_MAPPING_TRAITS(SourceLineEntry)
LLVM_YAML_DECLARE_MAPPING_TRAITS(SourceColumnEntry)
LLVM_YAML_DECLARE_MAPPING_TRAITS(SourceFileChecksumEntry)
LLVM_YAML_DECLARE_MAPPING_TRAITS(SourceLineBlock)
LLVM_YAML_DECLARE_MAPPING_TRAITS(InlineeSite)

namespace llvm {
namespace CodeViewYAML {
namespace detail {
struct YAMLSubsectionBase {
  explicit YAMLSubsectionBase(DebugSubsectionKind Kind) : Kind(Kind) {}
  DebugSubsectionKind Kind;
  virtual ~YAMLSubsectionBase() {}

  virtual void map(IO &IO) = 0;
  virtual std::unique_ptr<DebugSubsection>
  toCodeViewSubsection(DebugStringTableSubsection *UseStrings,
                       DebugChecksumsSubsection *UseChecksums) const = 0;
};
}
}
}

namespace {
struct YAMLChecksumsSubsection : public YAMLSubsectionBase {
  YAMLChecksumsSubsection()
      : YAMLSubsectionBase(DebugSubsectionKind::FileChecksums) {}

  void map(IO &IO) override;
  std::unique_ptr<DebugSubsection>
  toCodeViewSubsection(DebugStringTableSubsection *Strings,
                       DebugChecksumsSubsection *Checksums) const override;
  static Expected<std::shared_ptr<YAMLChecksumsSubsection>>
  fromCodeViewSubsection(const DebugStringTableSubsectionRef &Strings,
                         const DebugChecksumsSubsectionRef &FC);

  std::vector<SourceFileChecksumEntry> Checksums;
};

struct YAMLLinesSubsection : public YAMLSubsectionBase {
  YAMLLinesSubsection() : YAMLSubsectionBase(DebugSubsectionKind::Lines) {}

  void map(IO &IO) override;
  std::unique_ptr<DebugSubsection>
  toCodeViewSubsection(DebugStringTableSubsection *Strings,
                       DebugChecksumsSubsection *Checksums) const override;
  static Expected<std::shared_ptr<YAMLLinesSubsection>>
  fromCodeViewSubsection(const DebugStringTableSubsectionRef &Strings,
                         const DebugChecksumsSubsectionRef &Checksums,
                         const DebugLinesSubsectionRef &Lines);

  SourceLineInfo Lines;
};

struct YAMLInlineeLinesSubsection : public YAMLSubsectionBase {
  YAMLInlineeLinesSubsection()
      : YAMLSubsectionBase(DebugSubsectionKind::InlineeLines) {}

  void map(IO &IO) override;
  std::unique_ptr<DebugSubsection>
  toCodeViewSubsection(DebugStringTableSubsection *Strings,
                       DebugChecksumsSubsection *Checksums) const override;
  static Expected<std::shared_ptr<YAMLInlineeLinesSubsection>>
  fromCodeViewSubsection(const DebugStringTableSubsectionRef &Strings,
                         const DebugChecksumsSubsectionRef &Checksums,
                         const DebugInlineeLinesSubsectionRef &Lines);

  InlineeInfo InlineeLines;
};

struct YAMLCrossModuleExportsSubsection : public YAMLSubsectionBase {
  YAMLCrossModuleExportsSubsection()
      : YAMLSubsectionBase(DebugSubsectionKind::CrossScopeExports) {}

  void map(IO &IO) override;
  std::unique_ptr<DebugSubsection>
  toCodeViewSubsection(DebugStringTableSubsection *Strings,
                       DebugChecksumsSubsection *Checksums) const override;
  static Expected<std::shared_ptr<YAMLCrossModuleExportsSubsection>>
  fromCodeViewSubsection(const DebugCrossModuleExportsSubsectionRef &Exports);

  std::vector<CrossModuleExport> Exports;
};

struct YAMLCrossModuleImportsSubsection : public YAMLSubsectionBase {
  YAMLCrossModuleImportsSubsection()
      : YAMLSubsectionBase(DebugSubsectionKind::CrossScopeImports) {}

  void map(IO &IO) override;
  std::unique_ptr<DebugSubsection>
  toCodeViewSubsection(DebugStringTableSubsection *Strings,
                       DebugChecksumsSubsection *Checksums) const override;
  static Expected<std::shared_ptr<YAMLCrossModuleImportsSubsection>>
  fromCodeViewSubsection(const DebugStringTableSubsectionRef &Strings,
                         const DebugCrossModuleImportsSubsectionRef &Imports);

  std::vector<YAMLCrossModuleImport> Imports;
};
}

void ScalarBitSetTraits<LineFlags>::bitset(IO &io, LineFlags &Flags) {
  io.bitSetCase(Flags, "HasColumnInfo", LF_HaveColumns);
  io.enumFallback<Hex16>(Flags);
}

void ScalarEnumerationTraits<FileChecksumKind>::enumeration(
    IO &io, FileChecksumKind &Kind) {
  io.enumCase(Kind, "None", FileChecksumKind::None);
  io.enumCase(Kind, "MD5", FileChecksumKind::MD5);
  io.enumCase(Kind, "SHA1", FileChecksumKind::SHA1);
  io.enumCase(Kind, "SHA256", FileChecksumKind::SHA256);
}

void ScalarTraits<HexFormattedString>::output(const HexFormattedString &Value,
                                              void *ctx, raw_ostream &Out) {
  StringRef Bytes(reinterpret_cast<const char *>(Value.Bytes.data()),
                  Value.Bytes.size());
  Out << toHex(Bytes);
}

StringRef ScalarTraits<HexFormattedString>::input(StringRef Scalar, void *ctxt,
                                                  HexFormattedString &Value) {
  std::string H = fromHex(Scalar);
  Value.Bytes.assign(H.begin(), H.end());
  return StringRef();
}

void MappingTraits<SourceLineEntry>::mapping(IO &IO, SourceLineEntry &Obj) {
  IO.mapRequired("Offset", Obj.Offset);
  IO.mapRequired("LineStart", Obj.LineStart);
  IO.mapRequired("IsStatement", Obj.IsStatement);
  IO.mapRequired("EndDelta", Obj.EndDelta);
}

void MappingTraits<SourceColumnEntry>::mapping(IO &IO, SourceColumnEntry &Obj) {
  IO.mapRequired("StartColumn", Obj.StartColumn);
  IO.mapRequired("EndColumn", Obj.EndColumn);
}

void MappingTraits<SourceLineBlock>::mapping(IO &IO, SourceLineBlock &Obj) {
  IO.mapRequired("FileName", Obj.FileName);
  IO.mapRequired("Lines", Obj.Lines);
  IO.mapRequired("Columns", Obj.Columns);
}

void MappingTraits<CrossModuleExport>::mapping(IO &IO, CrossModuleExport &Obj) {
  IO.mapRequired("LocalId", Obj.Local);
  IO.mapRequired("GlobalId", Obj.Global);
}

void MappingTraits<YAMLCrossModuleImport>::mapping(IO &IO,
                                                   YAMLCrossModuleImport &Obj) {
  IO.mapRequired("Module", Obj.ModuleName);
  IO.mapRequired("Imports", Obj.ImportIds);
}

void MappingTraits<SourceFileChecksumEntry>::mapping(
    IO &IO, SourceFileChecksumEntry &Obj) {
  IO.mapRequired("FileName", Obj.FileName);
  IO.mapRequired("Kind", Obj.Kind);
  IO.mapRequired("Checksum", Obj.ChecksumBytes);
}

void MappingTraits<InlineeSite>::mapping(IO &IO, InlineeSite &Obj) {
  IO.mapRequired("FileName", Obj.FileName);
  IO.mapRequired("LineNum", Obj.SourceLineNum);
  IO.mapRequired("Inlinee", Obj.Inlinee);
  IO.mapOptional("ExtraFiles", Obj.ExtraFiles);
}

void YAMLChecksumsSubsection::map(IO &IO) {
  IO.mapTag("!FileChecksums", true);
  IO.mapRequired("Checksums", Checksums);
}

void YAMLLinesSubsection::map(IO &IO) {
  IO.mapTag("!Lines", true);
  IO.mapRequired("CodeSize", Lines.CodeSize);

  IO.mapRequired("Flags", Lines.Flags);
  IO.mapRequired("RelocOffset", Lines.RelocOffset);
  IO.mapRequired("RelocSegment", Lines.RelocSegment);
  IO.mapRequired("Blocks", Lines.Blocks);
}

void YAMLInlineeLinesSubsection::map(IO &IO) {
  IO.mapTag("!InlineeLines", true);
  IO.mapRequired("HasExtraFiles", InlineeLines.HasExtraFiles);
  IO.mapRequired("Sites", InlineeLines.Sites);
}

void YAMLCrossModuleExportsSubsection::map(IO &IO) {
  IO.mapTag("!CrossModuleExports", true);
  IO.mapOptional("Exports", Exports);
}

void YAMLCrossModuleImportsSubsection::map(IO &IO) {
  IO.mapTag("!CrossModuleImports", true);
  IO.mapOptional("Imports", Imports);
}

void MappingTraits<YAMLDebugSubsection>::mapping(
    IO &IO, YAMLDebugSubsection &Subsection) {
  if (!IO.outputting()) {
    if (IO.mapTag("!FileChecksums")) {
      auto SS = std::make_shared<YAMLChecksumsSubsection>();
      Subsection.Subsection = SS;
    } else if (IO.mapTag("!Lines")) {
      Subsection.Subsection = std::make_shared<YAMLLinesSubsection>();
    } else if (IO.mapTag("!InlineeLines")) {
      Subsection.Subsection = std::make_shared<YAMLInlineeLinesSubsection>();
    } else if (IO.mapTag("!CrossModuleExports")) {
      Subsection.Subsection =
          std::make_shared<YAMLCrossModuleExportsSubsection>();
    } else if (IO.mapTag("!CrossModuleImports")) {
      Subsection.Subsection =
          std::make_shared<YAMLCrossModuleImportsSubsection>();
    } else {
      llvm_unreachable("Unexpected subsection tag!");
    }
  }
  Subsection.Subsection->map(IO);
}

static std::shared_ptr<YAMLChecksumsSubsection>
findChecksums(ArrayRef<YAMLDebugSubsection> Subsections) {
  for (const auto &SS : Subsections) {
    if (SS.Subsection->Kind == DebugSubsectionKind::FileChecksums) {
      return std::static_pointer_cast<YAMLChecksumsSubsection>(SS.Subsection);
    }
  }

  return nullptr;
}

std::unique_ptr<DebugSubsection> YAMLChecksumsSubsection::toCodeViewSubsection(
    DebugStringTableSubsection *UseStrings,
    DebugChecksumsSubsection *UseChecksums) const {
  assert(UseStrings && !UseChecksums);
  auto Result = llvm::make_unique<DebugChecksumsSubsection>(*UseStrings);
  for (const auto &CS : Checksums) {
    Result->addChecksum(CS.FileName, CS.Kind, CS.ChecksumBytes.Bytes);
  }
  return std::move(Result);
}

std::unique_ptr<DebugSubsection> YAMLLinesSubsection::toCodeViewSubsection(
    DebugStringTableSubsection *UseStrings,
    DebugChecksumsSubsection *UseChecksums) const {
  assert(UseStrings && UseChecksums);
  auto Result =
      llvm::make_unique<DebugLinesSubsection>(*UseChecksums, *UseStrings);
  Result->setCodeSize(Lines.CodeSize);
  Result->setRelocationAddress(Lines.RelocSegment, Lines.RelocOffset);
  Result->setFlags(Lines.Flags);
  for (const auto &LC : Lines.Blocks) {
    Result->createBlock(LC.FileName);
    if (Result->hasColumnInfo()) {
      for (const auto &Item : zip(LC.Lines, LC.Columns)) {
        auto &L = std::get<0>(Item);
        auto &C = std::get<1>(Item);
        uint32_t LE = L.LineStart + L.EndDelta;
        Result->addLineAndColumnInfo(L.Offset,
                                     LineInfo(L.LineStart, LE, L.IsStatement),
                                     C.StartColumn, C.EndColumn);
      }
    } else {
      for (const auto &L : LC.Lines) {
        uint32_t LE = L.LineStart + L.EndDelta;
        Result->addLineInfo(L.Offset, LineInfo(L.LineStart, LE, L.IsStatement));
      }
    }
  }
  return llvm::cast<DebugSubsection>(std::move(Result));
}

std::unique_ptr<DebugSubsection>
YAMLInlineeLinesSubsection::toCodeViewSubsection(
    DebugStringTableSubsection *UseStrings,
    DebugChecksumsSubsection *UseChecksums) const {
  assert(UseChecksums);
  auto Result = llvm::make_unique<DebugInlineeLinesSubsection>(
      *UseChecksums, InlineeLines.HasExtraFiles);

  for (const auto &Site : InlineeLines.Sites) {
    Result->addInlineSite(TypeIndex(Site.Inlinee), Site.FileName,
                          Site.SourceLineNum);
    if (!InlineeLines.HasExtraFiles)
      continue;

    for (auto EF : Site.ExtraFiles) {
      Result->addExtraFile(EF);
    }
  }
  return llvm::cast<DebugSubsection>(std::move(Result));
}

std::unique_ptr<DebugSubsection>
YAMLCrossModuleExportsSubsection::toCodeViewSubsection(
    DebugStringTableSubsection *Strings,
    DebugChecksumsSubsection *Checksums) const {
  auto Result = llvm::make_unique<DebugCrossModuleExportsSubsection>();
  for (const auto &M : Exports)
    Result->addMapping(M.Local, M.Global);
  return llvm::cast<DebugSubsection>(std::move(Result));
}

std::unique_ptr<DebugSubsection>
YAMLCrossModuleImportsSubsection::toCodeViewSubsection(
    DebugStringTableSubsection *Strings,
    DebugChecksumsSubsection *Checksums) const {
  auto Result = llvm::make_unique<DebugCrossModuleImportsSubsection>(*Strings);
  for (const auto &M : Imports) {
    for (const auto Id : M.ImportIds)
      Result->addImport(M.ModuleName, Id);
  }
  return llvm::cast<DebugSubsection>(std::move(Result));
}

static Expected<SourceFileChecksumEntry>
convertOneChecksum(const DebugStringTableSubsectionRef &Strings,
                   const FileChecksumEntry &CS) {
  auto ExpectedString = Strings.getString(CS.FileNameOffset);
  if (!ExpectedString)
    return ExpectedString.takeError();

  SourceFileChecksumEntry Result;
  Result.ChecksumBytes.Bytes = CS.Checksum;
  Result.Kind = CS.Kind;
  Result.FileName = *ExpectedString;
  return Result;
}

static Expected<StringRef>
getFileName(const DebugStringTableSubsectionRef &Strings,
            const DebugChecksumsSubsectionRef &Checksums, uint32_t FileID) {
  auto Iter = Checksums.getArray().at(FileID);
  if (Iter == Checksums.getArray().end())
    return make_error<CodeViewError>(cv_error_code::no_records);
  uint32_t Offset = Iter->FileNameOffset;
  return Strings.getString(Offset);
}

Expected<std::shared_ptr<YAMLChecksumsSubsection>>
YAMLChecksumsSubsection::fromCodeViewSubsection(
    const DebugStringTableSubsectionRef &Strings,
    const DebugChecksumsSubsectionRef &FC) {
  auto Result = std::make_shared<YAMLChecksumsSubsection>();

  for (const auto &CS : FC) {
    auto ConvertedCS = convertOneChecksum(Strings, CS);
    if (!ConvertedCS)
      return ConvertedCS.takeError();
    Result->Checksums.push_back(*ConvertedCS);
  }
  return Result;
}

Expected<std::shared_ptr<YAMLLinesSubsection>>
YAMLLinesSubsection::fromCodeViewSubsection(
    const DebugStringTableSubsectionRef &Strings,
    const DebugChecksumsSubsectionRef &Checksums,
    const DebugLinesSubsectionRef &Lines) {
  auto Result = std::make_shared<YAMLLinesSubsection>();
  Result->Lines.CodeSize = Lines.header()->CodeSize;
  Result->Lines.RelocOffset = Lines.header()->RelocOffset;
  Result->Lines.RelocSegment = Lines.header()->RelocSegment;
  Result->Lines.Flags = static_cast<LineFlags>(uint16_t(Lines.header()->Flags));
  for (const auto &L : Lines) {
    SourceLineBlock Block;
    auto EF = getFileName(Strings, Checksums, L.NameIndex);
    if (!EF)
      return EF.takeError();
    Block.FileName = *EF;
    if (Lines.hasColumnInfo()) {
      for (const auto &C : L.Columns) {
        SourceColumnEntry SCE;
        SCE.EndColumn = C.EndColumn;
        SCE.StartColumn = C.StartColumn;
        Block.Columns.push_back(SCE);
      }
    }
    for (const auto &LN : L.LineNumbers) {
      SourceLineEntry SLE;
      LineInfo LI(LN.Flags);
      SLE.Offset = LN.Offset;
      SLE.LineStart = LI.getStartLine();
      SLE.EndDelta = LI.getLineDelta();
      SLE.IsStatement = LI.isStatement();
      Block.Lines.push_back(SLE);
    }
    Result->Lines.Blocks.push_back(Block);
  }
  return Result;
}

Expected<std::shared_ptr<YAMLInlineeLinesSubsection>>
YAMLInlineeLinesSubsection::fromCodeViewSubsection(
    const DebugStringTableSubsectionRef &Strings,
    const DebugChecksumsSubsectionRef &Checksums,
    const DebugInlineeLinesSubsectionRef &Lines) {
  auto Result = std::make_shared<YAMLInlineeLinesSubsection>();

  Result->InlineeLines.HasExtraFiles = Lines.hasExtraFiles();
  for (const auto &IL : Lines) {
    InlineeSite Site;
    auto ExpF = getFileName(Strings, Checksums, IL.Header->FileID);
    if (!ExpF)
      return ExpF.takeError();
    Site.FileName = *ExpF;
    Site.Inlinee = IL.Header->Inlinee.getIndex();
    Site.SourceLineNum = IL.Header->SourceLineNum;
    if (Lines.hasExtraFiles()) {
      for (const auto EF : IL.ExtraFiles) {
        auto ExpF2 = getFileName(Strings, Checksums, EF);
        if (!ExpF2)
          return ExpF2.takeError();
        Site.ExtraFiles.push_back(*ExpF2);
      }
    }
    Result->InlineeLines.Sites.push_back(Site);
  }
  return Result;
}

Expected<std::shared_ptr<YAMLCrossModuleExportsSubsection>>
YAMLCrossModuleExportsSubsection::fromCodeViewSubsection(
    const DebugCrossModuleExportsSubsectionRef &Exports) {
  auto Result = std::make_shared<YAMLCrossModuleExportsSubsection>();
  Result->Exports.assign(Exports.begin(), Exports.end());
  return Result;
}

Expected<std::shared_ptr<YAMLCrossModuleImportsSubsection>>
YAMLCrossModuleImportsSubsection::fromCodeViewSubsection(
    const DebugStringTableSubsectionRef &Strings,
    const DebugCrossModuleImportsSubsectionRef &Imports) {
  auto Result = std::make_shared<YAMLCrossModuleImportsSubsection>();
  for (const auto &CMI : Imports) {
    YAMLCrossModuleImport YCMI;
    auto ExpectedStr = Strings.getString(CMI.Header->ModuleNameOffset);
    if (!ExpectedStr)
      return ExpectedStr.takeError();
    YCMI.ModuleName = *ExpectedStr;
    YCMI.ImportIds.assign(CMI.Imports.begin(), CMI.Imports.end());
    Result->Imports.push_back(YCMI);
  }
  return Result;
}

Expected<std::vector<std::unique_ptr<DebugSubsection>>>
llvm::CodeViewYAML::convertSubsectionList(
    ArrayRef<YAMLDebugSubsection> Subsections,
    DebugStringTableSubsection &Strings) {
  std::vector<std::unique_ptr<DebugSubsection>> Result;
  if (Subsections.empty())
    return std::move(Result);

  auto Checksums = findChecksums(Subsections);
  std::unique_ptr<DebugSubsection> ChecksumsBase;
  if (Checksums)
    ChecksumsBase = Checksums->toCodeViewSubsection(&Strings, nullptr);
  DebugChecksumsSubsection *CS =
      static_cast<DebugChecksumsSubsection *>(ChecksumsBase.get());
  for (const auto &SS : Subsections) {
    // We've already converted the checksums subsection, don't do it
    // twice.
    std::unique_ptr<DebugSubsection> CVS;
    if (SS.Subsection->Kind == DebugSubsectionKind::FileChecksums)
      CVS = std::move(ChecksumsBase);
    else
      CVS = SS.Subsection->toCodeViewSubsection(&Strings, CS);
    assert(CVS != nullptr);
    Result.push_back(std::move(CVS));
  }
  return std::move(Result);
}

namespace {
struct SubsectionConversionVisitor : public DebugSubsectionVisitor {
  SubsectionConversionVisitor() {}

  Error visitUnknown(DebugUnknownSubsectionRef &Unknown) override;
  Error visitLines(DebugLinesSubsectionRef &Lines,
                   const DebugSubsectionState &State) override;
  Error visitFileChecksums(DebugChecksumsSubsectionRef &Checksums,
                           const DebugSubsectionState &State) override;
  Error visitInlineeLines(DebugInlineeLinesSubsectionRef &Inlinees,
                          const DebugSubsectionState &State) override;
  Error visitCrossModuleExports(DebugCrossModuleExportsSubsectionRef &Checksums,
                                const DebugSubsectionState &State) override;
  Error visitCrossModuleImports(DebugCrossModuleImportsSubsectionRef &Inlinees,
                                const DebugSubsectionState &State) override;

  YAMLDebugSubsection Subsection;
};

Error SubsectionConversionVisitor::visitUnknown(
    DebugUnknownSubsectionRef &Unknown) {
  return make_error<CodeViewError>(cv_error_code::operation_unsupported);
}

Error SubsectionConversionVisitor::visitLines(
    DebugLinesSubsectionRef &Lines, const DebugSubsectionState &State) {
  auto Result = YAMLLinesSubsection::fromCodeViewSubsection(
      State.strings(), State.checksums(), Lines);
  if (!Result)
    return Result.takeError();
  Subsection.Subsection = *Result;
  return Error::success();
}

Error SubsectionConversionVisitor::visitFileChecksums(
    DebugChecksumsSubsectionRef &Checksums, const DebugSubsectionState &State) {
  auto Result = YAMLChecksumsSubsection::fromCodeViewSubsection(State.strings(),
                                                                Checksums);
  if (!Result)
    return Result.takeError();
  Subsection.Subsection = *Result;
  return Error::success();
}

Error SubsectionConversionVisitor::visitInlineeLines(
    DebugInlineeLinesSubsectionRef &Inlinees,
    const DebugSubsectionState &State) {
  auto Result = YAMLInlineeLinesSubsection::fromCodeViewSubsection(
      State.strings(), State.checksums(), Inlinees);
  if (!Result)
    return Result.takeError();
  Subsection.Subsection = *Result;
  return Error::success();
}

Error SubsectionConversionVisitor::visitCrossModuleExports(
    DebugCrossModuleExportsSubsectionRef &Exports,
    const DebugSubsectionState &State) {
  auto Result =
      YAMLCrossModuleExportsSubsection::fromCodeViewSubsection(Exports);
  if (!Result)
    return Result.takeError();
  Subsection.Subsection = *Result;
  return Error::success();
}

Error SubsectionConversionVisitor::visitCrossModuleImports(
    DebugCrossModuleImportsSubsectionRef &Imports,
    const DebugSubsectionState &State) {
  auto Result = YAMLCrossModuleImportsSubsection::fromCodeViewSubsection(
      State.strings(), Imports);
  if (!Result)
    return Result.takeError();
  Subsection.Subsection = *Result;
  return Error::success();
}
}

Expected<YAMLDebugSubsection> YAMLDebugSubsection::fromCodeViewSubection(
    const DebugStringTableSubsectionRef &Strings,
    const DebugChecksumsSubsectionRef &Checksums,
    const DebugSubsectionRecord &SS) {
  DebugSubsectionState State(Strings, Checksums);
  SubsectionConversionVisitor V;
  if (auto EC = visitDebugSubsection(SS, V, State))
    return std::move(EC);

  return V.Subsection;
}
