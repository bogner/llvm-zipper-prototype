//===- Config.h -------------------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_CONFIG_H
#define LLD_ELF_CONFIG_H

#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ELF.h"

#include <vector>

namespace lld {
namespace elf {

class InputFile;
struct Symbol;

enum ELFKind {
  ELFNoneKind,
  ELF32LEKind,
  ELF32BEKind,
  ELF64LEKind,
  ELF64BEKind
};

// For --build-id.
enum class BuildIdKind { None, Fast, Md5, Sha1, Hexstring, Uuid };

// For --discard-{all,locals,none}.
enum class DiscardPolicy { Default, All, Locals, None };

// For --strip-{all,debug}.
enum class StripPolicy { None, All, Debug };

// For --unresolved-symbols.
enum class UnresolvedPolicy { NoUndef, ReportError, Warn, Ignore };

// For --sort-section and linkerscript sorting rules.
enum class SortSectionPolicy { Default, None, Alignment, Name, Priority };

// For --target2
enum class Target2Policy { Abs, Rel, GotRel };

struct SymbolVersion {
  llvm::StringRef Name;
  bool IsExternCpp;
  bool HasWildcards;
};

// This struct contains symbols version definition that
// can be found in version script if it is used for link.
struct VersionDefinition {
  VersionDefinition(llvm::StringRef Name, size_t Id) : Name(Name), Id(Id) {}
  llvm::StringRef Name;
  size_t Id;
  std::vector<SymbolVersion> Globals;
  std::vector<SymbolVersion> Locals;
  size_t NameOff; // Offset in string table.
};

// This struct contains the global configuration for the linker.
// Most fields are direct mapping from the command line options
// and such fields have the same name as the corresponding options.
// Most fields are initialized by the driver.
struct Configuration {
  InputFile *FirstElf = nullptr;
  uint8_t OSABI = 0;
  llvm::DenseMap<llvm::CachedHashStringRef, unsigned> SymbolOrderingFile;
  llvm::StringMap<uint64_t> SectionStartMap;
  llvm::StringRef DynamicLinker;
  llvm::StringRef Entry;
  llvm::StringRef Emulation;
  llvm::StringRef Fini;
  llvm::StringRef Init;
  llvm::StringRef LtoAAPipeline;
  llvm::StringRef LtoNewPmPasses;
  llvm::StringRef OutputFile;
  llvm::StringRef SoName;
  llvm::StringRef Sysroot;
  std::string RPath;
  std::vector<VersionDefinition> VersionDefinitions;
  std::vector<llvm::StringRef> AuxiliaryList;
  std::vector<llvm::StringRef> DynamicList;
  std::vector<llvm::StringRef> SearchPaths;
  std::vector<llvm::StringRef> Undefined;
  std::vector<SymbolVersion> VersionScriptGlobals;
  std::vector<uint8_t> BuildIdVector;
  bool AllowMultipleDefinition;
  bool AsNeeded = false;
  bool Bsymbolic;
  bool BsymbolicFunctions;
  bool Demangle = true;
  bool DisableVerify;
  bool EhFrameHdr;
  bool EnableNewDtags;
  bool ExportDynamic;
  bool FatalWarnings;
  bool GcSections;
  bool GdbIndex;
  bool GnuHash = false;
  bool ICF;
  bool Mips64EL = false;
  bool MipsN32Abi = false;
  bool NoGnuUnique;
  bool NoUndefinedVersion;
  bool Nostdlib;
  bool OFormatBinary;
  bool Pic;
  bool Pie;
  bool PrintGcSections;
  bool Rela;
  bool Relocatable;
  bool SaveTemps;
  bool Shared;
  bool Static = false;
  bool SysvHash = true;
  bool Target1Rel;
  bool Threads;
  bool Trace;
  bool Verbose;
  bool WarnCommon;
  bool ZCombreloc;
  bool ZExecstack;
  bool ZNodelete;
  bool ZNow;
  bool ZOrigin;
  bool ZRelro;
  bool ExitEarly;
  bool ZWxneeded;
  DiscardPolicy Discard;
  SortSectionPolicy SortSection;
  StripPolicy Strip = StripPolicy::None;
  UnresolvedPolicy UnresolvedSymbols;
  Target2Policy Target2 = Target2Policy::GotRel;
  BuildIdKind BuildId = BuildIdKind::None;
  ELFKind EKind = ELFNoneKind;
  uint16_t DefaultSymbolVersion = llvm::ELF::VER_NDX_GLOBAL;
  uint16_t EMachine = llvm::ELF::EM_NONE;
  uint64_t EntryAddr = 0;
  uint64_t ImageBase;
  uint64_t MaxPageSize;
  uint64_t ZStackSize;
  unsigned LtoPartitions;
  unsigned LtoO;
  unsigned Optimize;
  unsigned ThinLtoJobs;
};

// The only instance of Configuration struct.
extern Configuration *Config;

} // namespace elf
} // namespace lld

#endif
