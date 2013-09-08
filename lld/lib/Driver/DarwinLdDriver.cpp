//===- lib/Driver/DarwinLdDriver.cpp --------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Concrete instance of the Driver for darwin's ld.
///
//===----------------------------------------------------------------------===//

#include "lld/Driver/Driver.h"
#include "lld/Driver/DarwinInputGraph.h"
#include "lld/ReaderWriter/MachOLinkingContext.h"
#include "lld/ReaderWriter/MachOFormat.hpp"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"


namespace {

// Create enum with OPT_xxx values for each option in DarwinLdOptions.td
enum {
  OPT_INVALID = 0,
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELP, META) \
          OPT_##ID,
#include "DarwinLdOptions.inc"
#undef OPTION
};

// Create prefix string literals used in DarwinLdOptions.td
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "DarwinLdOptions.inc"
#undef PREFIX

// Create table mapping all options defined in DarwinLdOptions.td
static const llvm::opt::OptTable::Info infoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELPTEXT, METAVAR)   \
  { PREFIX, NAME, HELPTEXT, METAVAR, OPT_##ID, llvm::opt::Option::KIND##Class, \
    PARAM, FLAGS, OPT_##GROUP, OPT_##ALIAS, ALIASARGS },
#include "DarwinLdOptions.inc"
#undef OPTION
};

// Create OptTable class for parsing actual command line arguments
class DarwinLdOptTable : public llvm::opt::OptTable {
public:
  DarwinLdOptTable() : OptTable(infoTable, llvm::array_lengthof(infoTable)){}
};


} // namespace anonymous

namespace lld {

llvm::ErrorOr<std::unique_ptr<lld::LinkerInput> >
MachOFileNode::createLinkerInput(const LinkingContext &ctx) {
  auto inputFile(FileNode::createLinkerInput(ctx));

  if (inputFile)
    (*inputFile)->setWholeArchive(false);
  return std::move(inputFile);
}

bool DarwinLdDriver::linkMachO(int argc, const char *argv[],
                               raw_ostream &diagnostics) {
  MachOLinkingContext ctx;
  if (parse(argc, argv, ctx, diagnostics))
    return true;
  if (ctx.doNothing())
    return false;

  return link(ctx, diagnostics);
}

bool DarwinLdDriver::parse(int argc, const char *argv[],
                           MachOLinkingContext &ctx, raw_ostream &diagnostics) {
  // Parse command line options using DarwinLdOptions.td
  std::unique_ptr<llvm::opt::InputArgList> parsedArgs;
  DarwinLdOptTable table;
  unsigned missingIndex;
  unsigned missingCount;
  bool globalWholeArchive = false;
  parsedArgs.reset(
      table.ParseArgs(&argv[1], &argv[argc], missingIndex, missingCount));
  if (missingCount) {
    diagnostics << "error: missing arg value for '"
                << parsedArgs->getArgString(missingIndex) << "' expected "
                << missingCount << " argument(s).\n";
    return true;
  }

  for (auto it = parsedArgs->filtered_begin(OPT_UNKNOWN),
            ie = parsedArgs->filtered_end(); it != ie; ++it) {
    diagnostics  << "warning: ignoring unknown argument: "
                 << (*it)->getAsString(*parsedArgs) << "\n";
  }

  // Figure out output kind ( -dylib, -r, -bundle, -preload, or -static )
  if ( llvm::opt::Arg *kind = parsedArgs->getLastArg(OPT_dylib, OPT_relocatable,
                                      OPT_bundle, OPT_static, OPT_preload)) {
    switch (kind->getOption().getID()) {
    case OPT_dylib:
      ctx.setOutputFileType(mach_o::MH_DYLIB);
      ctx.setGlobalsAreDeadStripRoots(true);
      break;
    case OPT_relocatable:
      ctx.setPrintRemainingUndefines(false);
      ctx.setAllowRemainingUndefines(true);
      ctx.setOutputFileType(mach_o::MH_OBJECT);
      break;
    case OPT_bundle:
      ctx.setOutputFileType(mach_o::MH_BUNDLE);
      break;
    case OPT_static:
      ctx.setOutputFileType(mach_o::MH_EXECUTE);
      break;
    case OPT_preload:
      ctx.setOutputFileType(mach_o::MH_PRELOAD);
      break;
    }
  }

  // Handle -e xxx
  if (llvm::opt::Arg *entry = parsedArgs->getLastArg(OPT_entry))
    ctx.setEntrySymbolName(entry->getValue());

  // Handle -o xxx
  if (llvm::opt::Arg *outpath = parsedArgs->getLastArg(OPT_output))
    ctx.setOutputPath(outpath->getValue());

  // Handle -dead_strip
  if (parsedArgs->getLastArg(OPT_dead_strip))
    ctx.setDeadStripping(true);

  // Handle -all_load
  if (parsedArgs->getLastArg(OPT_all_load))
    globalWholeArchive = true;

  // Handle -arch xxx
  if (llvm::opt::Arg *archStr = parsedArgs->getLastArg(OPT_arch)) {
    ctx.setArch(MachOLinkingContext::archFromName(archStr->getValue()));
    if (ctx.arch() == MachOLinkingContext::arch_unknown) {
      diagnostics << "error: unknown arch named '" << archStr->getValue()
                  << "'\n";
      return true;
    }
  }

  // Handle -macosx_version_min or -ios_version_min
  if (llvm::opt::Arg *minOS = parsedArgs->getLastArg(
                                               OPT_macosx_version_min,
                                               OPT_ios_version_min,
                                               OPT_ios_simulator_version_min)) {
    switch (minOS->getOption().getID()) {
    case OPT_macosx_version_min:
      if (ctx.setOS(MachOLinkingContext::OS::macOSX, minOS->getValue())) {
        diagnostics << "error: malformed macosx_version_min value\n";
        return true;
      }
      break;
    case OPT_ios_version_min:
      if (ctx.setOS(MachOLinkingContext::OS::iOS, minOS->getValue())) {
        diagnostics << "error: malformed ios_version_min value\n";
        return true;
      }
      break;
    case OPT_ios_simulator_version_min:
      if (ctx.setOS(MachOLinkingContext::OS::iOS_simulator,
                    minOS->getValue())) {
        diagnostics << "error: malformed ios_simulator_version_min value\n";
        return true;
      }
      break;
    }
  }
  else {
    // No min-os version on command line, check environment variables
  }

  std::unique_ptr<InputGraph> inputGraph(new InputGraph());

  // Handle input files
  for (llvm::opt::arg_iterator it = parsedArgs->filtered_begin(OPT_INPUT),
                               ie = parsedArgs->filtered_end();
                              it != ie; ++it) {
    inputGraph->addInputElement(std::unique_ptr<InputElement>(
        new MachOFileNode(ctx, (*it)->getValue(), globalWholeArchive)));
  }

  if (!inputGraph->numFiles()) {
    diagnostics << "No input files\n";
    return true;
  }

  ctx.setInputGraph(std::move(inputGraph));

  // Handle -help
  if (parsedArgs->getLastArg(OPT_help)) {
    table.PrintHelp(llvm::outs(), argv[0], "LLVM Darwin Linker", false);
    // If only -help on command line, don't try to do any linking
    if ( argc == 2 ) {
      ctx.setDoNothing(true);
      return false;
    }
  }

  // Validate the combination of options used.
  if (ctx.validate(diagnostics))
    return true;

  return false;
}

} // namespace lld
