//===- LTO.cpp ------------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "LTO.h"
#include "Config.h"
#include "InputFiles.h"
#include "LinkerScript.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/TargetOptionsCommandFlags.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/LTO/Caching.h"
#include "llvm/LTO/Config.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

using namespace lld;
using namespace lld::elf;

// This is for use when debugging LTO.
static void saveBuffer(StringRef Buffer, const Twine &Path) {
  std::error_code EC;
  raw_fd_ostream OS(Path.str(), EC, sys::fs::OpenFlags::F_None);
  if (EC)
    error("cannot create " + Path + ": " + EC.message());
  OS << Buffer;
}

static void diagnosticHandler(const DiagnosticInfo &DI) {
  SmallString<128> S;
  raw_svector_ostream OS(S);
  DiagnosticPrinterRawOStream DP(OS);
  DI.print(DP);
  warn(S);
}

static void checkError(Error E) {
  handleAllErrors(std::move(E),
                  [&](ErrorInfoBase &EIB) { error(EIB.message()); });
}

// Creates an empty file to store a list of object files for final
// linking of distributed ThinLTO.
static std::unique_ptr<raw_fd_ostream> openFile(StringRef File) {
  std::error_code EC;
  auto Ret =
      llvm::make_unique<raw_fd_ostream>(File, EC, sys::fs::OpenFlags::F_None);
  if (EC) {
    error("cannot open " + File + ": " + EC.message());
    return nullptr;
  }
  return Ret;
}

static std::string getThinLTOOutputFile(StringRef ModulePath) {
  return lto::getThinLTOOutputFile(ModulePath,
                                   Config->ThinLTOPrefixReplace.first,
                                   Config->ThinLTOPrefixReplace.second);
}

static lto::Config createConfig() {
  lto::Config C;

  // LLD supports the new relocations.
  C.Options = InitTargetOptionsFromCodeGenFlags();
  C.Options.RelaxELFRelocations = true;

  // Always emit a section per function/datum with LTO.
  C.Options.FunctionSections = true;
  C.Options.DataSections = true;

  if (Config->Relocatable)
    C.RelocModel = None;
  else if (Config->Pic)
    C.RelocModel = Reloc::PIC_;
  else
    C.RelocModel = Reloc::Static;

  C.CodeModel = GetCodeModelFromCMModel();
  C.DisableVerify = Config->DisableVerify;
  C.DiagHandler = diagnosticHandler;
  C.OptLevel = Config->LTOO;
  C.CPU = GetCPUStr();

  // Set up a custom pipeline if we've been asked to.
  C.OptPipeline = Config->LTONewPmPasses;
  C.AAPipeline = Config->LTOAAPipeline;

  // Set up optimization remarks if we've been asked to.
  C.RemarksFilename = Config->OptRemarksFilename;
  C.RemarksWithHotness = Config->OptRemarksWithHotness;

  C.SampleProfile = Config->LTOSampleProfile;
  C.UseNewPM = Config->LTONewPassManager;
  C.DebugPassManager = Config->LTODebugPassManager;

  if (Config->SaveTemps)
    checkError(C.addSaveTemps(Config->OutputFile.str() + ".",
                              /*UseInputModulePath*/ true));
  return C;
}

BitcodeCompiler::BitcodeCompiler() {
  // Initialize LTOObj.
  lto::ThinBackend Backend;

  if (Config->ThinLTOIndexOnly) {
    StringRef Path = Config->ThinLTOIndexOnlyArg;
    if (!Path.empty())
      IndexFile = openFile(Path);

    Backend = lto::createWriteIndexesThinBackend(
        Config->ThinLTOPrefixReplace.first, Config->ThinLTOPrefixReplace.second,
        Config->ThinLTOEmitImportsFiles, IndexFile.get(), nullptr);
  } else if (Config->ThinLTOJobs != -1U) {
    Backend = lto::createInProcessThinBackend(Config->ThinLTOJobs);
  }

  LTOObj = llvm::make_unique<lto::LTO>(createConfig(), Backend,
                                       Config->LTOPartitions);

  // Initialize UsedStartStop.
  for (Symbol *Sym : Symtab->getSymbols()) {
    StringRef Name = Sym->getName();
    for (StringRef Prefix : {"__start_", "__stop_"})
      if (Name.startswith(Prefix))
        UsedStartStop.insert(Name.substr(Prefix.size()));
  }
}

BitcodeCompiler::~BitcodeCompiler() = default;

static void undefine(Symbol *S) {
  replaceSymbol<Undefined>(S, nullptr, S->getName(), STB_GLOBAL, STV_DEFAULT,
                           S->Type);
}

void BitcodeCompiler::add(BitcodeFile &F) {
  lto::InputFile &Obj = *F.Obj;

  // Create the empty files which, if indexed, will be overwritten later.
  if (Config->ThinLTOIndexOnly) {
    std::string Path = getThinLTOOutputFile(Obj.getName());
    openFile(Path + ".thinlto.bc");

    if (Config->ThinLTOEmitImportsFiles)
      openFile(Path + ".imports");
  }

  unsigned SymNum = 0;
  std::vector<Symbol *> Syms = F.getSymbols();
  std::vector<lto::SymbolResolution> Resols(Syms.size());

  bool IsExecutable = !Config->Shared && !Config->Relocatable;

  // Provide a resolution to the LTO API for each symbol.
  for (const lto::InputFile::Symbol &ObjSym : Obj.symbols()) {
    Symbol *Sym = Syms[SymNum];
    lto::SymbolResolution &R = Resols[SymNum];
    ++SymNum;

    // Ideally we shouldn't check for SF_Undefined but currently IRObjectFile
    // reports two symbols for module ASM defined. Without this check, lld
    // flags an undefined in IR with a definition in ASM as prevailing.
    // Once IRObjectFile is fixed to report only one symbol this hack can
    // be removed.
    R.Prevailing = !ObjSym.isUndefined() && Sym->File == &F;

    // We ask LTO to preserve following global symbols:
    // 1) All symbols when doing relocatable link, so that them can be used
    //    for doing final link.
    // 2) Symbols that are used in regular objects.
    // 3) C named sections if we have corresponding __start_/__stop_ symbol.
    // 4) Symbols that are defined in bitcode files and used for dynamic linking.
    R.VisibleToRegularObj = Config->Relocatable || Sym->IsUsedInRegularObj ||
                            (R.Prevailing && Sym->includeInDynsym()) ||
                            UsedStartStop.count(ObjSym.getSectionName());
    const auto *DR = dyn_cast<Defined>(Sym);
    R.FinalDefinitionInLinkageUnit =
        (IsExecutable || Sym->Visibility != STV_DEFAULT) && DR &&
        // Skip absolute symbols from ELF objects, otherwise PC-rel relocations
        // will be generated by for them, triggering linker errors.
        // Symbol section is always null for bitcode symbols, hence the check
        // for isElf(). Skip linker script defined symbols as well: they have
        // no File defined.
        !(DR->Section == nullptr && (!Sym->File || Sym->File->isElf()));

    if (R.Prevailing)
      undefine(Sym);

    // We tell LTO to not apply interprocedural optimization for wrapped
    // (with --wrap) symbols because otherwise LTO would inline them while
    // their values are still not final.
    R.LinkerRedefined = !Sym->CanInline;
  }
  checkError(LTOObj->add(std::move(F.Obj), Resols));
}

// Merge all the bitcode files we have seen, codegen the result
// and return the resulting ObjectFile(s).
std::vector<InputFile *> BitcodeCompiler::compile() {
  std::vector<InputFile *> Ret;
  unsigned MaxTasks = LTOObj->getMaxTasks();
  Buff.resize(MaxTasks);
  Files.resize(MaxTasks);

  // The --thinlto-cache-dir option specifies the path to a directory in which
  // to cache native object files for ThinLTO incremental builds. If a path was
  // specified, configure LTO to use it as the cache directory.
  lto::NativeObjectCache Cache;
  if (!Config->ThinLTOCacheDir.empty())
    Cache = check(
        lto::localCache(Config->ThinLTOCacheDir,
                        [&](size_t Task, std::unique_ptr<MemoryBuffer> MB) {
                          Files[Task] = std::move(MB);
                        }));

  checkError(LTOObj->run(
      [&](size_t Task) {
        return llvm::make_unique<lto::NativeObjectStream>(
            llvm::make_unique<raw_svector_ostream>(Buff[Task]));
      },
      Cache));

  if (!Config->ThinLTOCacheDir.empty())
    pruneCache(Config->ThinLTOCacheDir, Config->ThinLTOCachePolicy);

  for (unsigned I = 0; I != MaxTasks; ++I) {
    if (Buff[I].empty())
      continue;
    if (Config->SaveTemps) {
      if (I == 0)
        saveBuffer(Buff[I], Config->OutputFile + ".lto.o");
      else
        saveBuffer(Buff[I], Config->OutputFile + Twine(I) + ".lto.o");
    }
    InputFile *Obj = createObjectFile(MemoryBufferRef(Buff[I], "lto.tmp"));
    Ret.push_back(Obj);
  }

  // If LazyObjFile has not been added to link, emit empty index files.
  // This is needed because this is what GNU gold plugin does and we have a
  // distributed build system that depends on that behavior.
  if (Config->ThinLTOIndexOnly) {
    for (LazyObjFile *F : LazyObjFiles) {
      if (F->AddedToLink || !isBitcode(F->MB))
        continue;

      std::string Path = getThinLTOOutputFile(F->getName());
      std::unique_ptr<raw_fd_ostream> OS = openFile(Path + ".thinlto.bc");
      if (!OS)
        continue;

      ModuleSummaryIndex M(false);
      M.setSkipModuleByDistributedBackend();
      WriteIndexToFile(M, *OS);

      if (Config->ThinLTOEmitImportsFiles)
        openFile(Path + ".imports");
    }

    // ThinLTO with index only option is required to generate only the index
    // files. After that, we exit from linker and ThinLTO backend runs in a
    // distributed environment.
    if (IndexFile)
      IndexFile->close();
    return {};
  }

  for (std::unique_ptr<MemoryBuffer> &File : Files)
    if (File)
      Ret.push_back(createObjectFile(*File));
  return Ret;
}
