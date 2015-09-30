//===- Driver.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Driver.h"
#include "Config.h"
#include "Error.h"
#include "InputFiles.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace llvm;

using namespace lld;
using namespace lld::elf2;

namespace lld {
namespace elf2 {

Configuration *Config;
std::vector<std::unique_ptr<MemoryBuffer>> *MemoryBufferPool;

void link(ArrayRef<const char *> Args) {
  Configuration C;
  Config = &C;
  std::vector<std::unique_ptr<MemoryBuffer>> V;
  MemoryBufferPool = &V;
  LinkerDriver().link(Args.slice(1));
}

// Opens a file. Path has to be resolved already.
// Newly created memory buffers are owned by this driver.
MemoryBufferRef openFile(StringRef Path) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> MBOrErr = MemoryBuffer::getFile(Path);
  error(MBOrErr, Twine("cannot open ") + Path);
  std::unique_ptr<MemoryBuffer> &MB = *MBOrErr;
  MemoryBufferRef MBRef = MB->getMemBufferRef();
  MemoryBufferPool->push_back(std::move(MB)); // transfer ownership
  return MBRef;
}
}
}

// Makes a path by concatenating Dir and File.
// If Dir starts with '=' the result will be preceded by Sysroot,
// which can be set with --sysroot command line switch.
static std::string buildSysrootedPath(StringRef Dir, StringRef File) {
  SmallString<128> Path;
  if (Dir.startswith("="))
    sys::path::append(Path, Config->Sysroot, Dir.substr(1), File);
  else
    sys::path::append(Path, Dir, File);
  return Path.str().str();
}

// Searches a given library from input search paths, which are filled
// from -L command line switches. Returns a path to an existent library file.
static std::string searchLibrary(StringRef Path) {
  std::vector<std::string> Names;
  if (Path[0] == ':') {
    Names.push_back(Path.drop_front().str());
  } else {
    Names.push_back((Twine("lib") + Path + ".so").str());
    Names.push_back((Twine("lib") + Path + ".a").str());
  }
  for (StringRef Dir : Config->InputSearchPaths) {
    for (const std::string &Name : Names) {
      std::string FullPath = buildSysrootedPath(Dir, Name);
      if (sys::fs::exists(FullPath))
        return FullPath;
    }
  }
  error(Twine("Unable to find library -l") + Path);
}

// Returns true if MB looks like a linker script.
static bool isLinkerScript(MemoryBufferRef MB) {
  using namespace llvm::sys::fs;
  return identify_magic(MB.getBuffer()) == file_magic::unknown;
}

void LinkerDriver::link(ArrayRef<const char *> ArgsArr) {
  // Parse command line options.
  opt::InputArgList Args = Parser.parse(ArgsArr);

  if (auto *Arg = Args.getLastArg(OPT_output))
    Config->OutputFile = Arg->getValue();

  if (auto *Arg = Args.getLastArg(OPT_dynamic_linker))
    Config->DynamicLinker = Arg->getValue();

  if (auto *Arg = Args.getLastArg(OPT_sysroot))
    Config->Sysroot = Arg->getValue();

  std::vector<StringRef> RPaths;
  for (auto *Arg : Args.filtered(OPT_rpath))
    RPaths.push_back(Arg->getValue());
  if (!RPaths.empty())
    Config->RPath = llvm::join(RPaths.begin(), RPaths.end(), ":");

  for (auto *Arg : Args.filtered(OPT_L))
    Config->InputSearchPaths.push_back(Arg->getValue());

  if (auto *Arg = Args.getLastArg(OPT_entry))
    Config->Entry = Arg->getValue();

  Config->AllowMultipleDefinition = Args.hasArg(OPT_allow_multiple_definition);
  Config->DiscardAll = Args.hasArg(OPT_discard_all);
  Config->DiscardLocals = Args.hasArg(OPT_discard_locals);
  Config->DiscardNone = Args.hasArg(OPT_discard_none);
  Config->ExportDynamic = Args.hasArg(OPT_export_dynamic);
  Config->NoInhibitExec = Args.hasArg(OPT_noinhibit_exec);
  Config->Shared = Args.hasArg(OPT_shared);

  // Create a symbol table.
  SymbolTable Symtab;

  for (auto *Arg : Args.filtered(OPT_l, OPT_INPUT)) {
    std::string Path = Arg->getValue();
    if (Arg->getOption().getID() == OPT_l)
      Path = searchLibrary(Path);
    MemoryBufferRef MB = openFile(Path);
    if (isLinkerScript(MB)) {
      // readLinkerScript may add files to the symbol table.
      readLinkerScript(&Symtab, MB);
      continue;
    }
    Symtab.addFile(createFile(MB));
  }

  if (Symtab.getObjectFiles().empty())
    error("no input files.");

  // Write the result.
  const ELFFileBase *FirstObj = Symtab.getFirstELF();
  switch (FirstObj->getELFKind()) {
  case ELF32LEKind:
    writeResult<object::ELF32LE>(&Symtab);
    return;
  case ELF32BEKind:
    writeResult<object::ELF32BE>(&Symtab);
    return;
  case ELF64LEKind:
    writeResult<object::ELF64LE>(&Symtab);
    return;
  case ELF64BEKind:
    writeResult<object::ELF64BE>(&Symtab);
    return;
  }
}
