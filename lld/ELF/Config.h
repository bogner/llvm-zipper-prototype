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

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ELF.h"

#include <vector>

namespace lld {
namespace elf {

class InputFile;
class SymbolBody;

enum ELFKind {
  ELFNoneKind,
  ELF32LEKind,
  ELF32BEKind,
  ELF64LEKind,
  ELF64BEKind
};

// This struct contains the global configuration for the linker.
// Most fields are direct mapping from the command line options
// and such fields have the same name as the corresponding options.
// Most fields are initialized by the driver.
struct Configuration {
  SymbolBody *EntrySym = nullptr;
  SymbolBody *MipsGpDisp = nullptr;
  SymbolBody *MipsLocalGp = nullptr;
  InputFile *FirstElf = nullptr;
  llvm::StringRef DynamicLinker;
  llvm::StringRef Entry;
  llvm::StringRef Emulation;
  llvm::StringRef Fini;
  llvm::StringRef Init;
  llvm::StringRef OutputFile;
  llvm::StringRef SoName;
  llvm::StringRef Sysroot;
  std::string RPath;
  std::vector<llvm::StringRef> SearchPaths;
  std::vector<llvm::StringRef> Undefined;
  bool AllowMultipleDefinition;
  bool AsNeeded = false;
  bool Bsymbolic;
  bool BsymbolicFunctions;
  bool Demangle = true;
  bool DiscardAll;
  bool DiscardLocals;
  bool DiscardNone;
  bool EhFrameHdr;
  bool EnableNewDtags;
  bool ExportDynamic;
  bool GcSections;
  bool GnuHash = false;
  bool ICF;
  bool Mips64EL = false;
  bool NoUndefined;
  bool NoinhibitExec;
  bool PrintGcSections;
  bool Relocatable;
  bool SaveTemps;
  bool Shared;
  bool Static = false;
  bool StripAll;
  bool SysvHash = true;
  bool Threads;
  bool Verbose;
  bool ZExecStack;
  bool ZNodelete;
  bool ZNow;
  bool ZOrigin;
  bool ZRelro;
  ELFKind EKind = ELFNoneKind;
  uint16_t EMachine = llvm::ELF::EM_NONE;
  uint64_t EntryAddr = -1;
  unsigned Optimize = 0;
};

// The only instance of Configuration struct.
extern Configuration *Config;

} // namespace elf
} // namespace lld

#endif
