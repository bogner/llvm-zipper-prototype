//===- lib/Driver/WinLinkDriver.cpp ---------------------------------------===//
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
/// Concrete instance of the Driver for Windows link.exe.
///
//===----------------------------------------------------------------------===//

#include <cstdlib>
#include <sstream>
#include <map>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Path.h"

#include "lld/Driver/Driver.h"
#include "lld/ReaderWriter/PECOFFLinkingContext.h"

namespace lld {

namespace {

// Create enum with OPT_xxx values for each option in WinLinkOptions.td
enum WinLinkOpt {
  OPT_INVALID = 0,
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELP, META) \
          OPT_##ID,
#include "WinLinkOptions.inc"
  LastOption
#undef OPTION
};

// Create prefix string literals used in WinLinkOptions.td
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "WinLinkOptions.inc"
#undef PREFIX

// Create table mapping all options defined in WinLinkOptions.td
static const llvm::opt::OptTable::Info infoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELPTEXT, METAVAR)   \
  { PREFIX, NAME, HELPTEXT, METAVAR, OPT_##ID, llvm::opt::Option::KIND##Class, \
    PARAM, FLAGS, OPT_##GROUP, OPT_##ALIAS, ALIASARGS },
#include "WinLinkOptions.inc"
#undef OPTION
};

// Create OptTable class for parsing actual command line arguments
class WinLinkOptTable : public llvm::opt::OptTable {
public:
  WinLinkOptTable() : OptTable(infoTable, llvm::array_lengthof(infoTable)){}
};

// Displays error message if the given version does not match with
// /^\d+$/.
bool checkNumber(StringRef version, const char *errorMessage,
                 raw_ostream &diagnostics) {
  if (version.str().find_first_not_of("0123456789") != std::string::npos
      || version.empty()) {
    diagnostics << "error: " << errorMessage << version << "\n";
    return false;
  }
  return true;
}

// Parse an argument for /base, /stack or /heap. The expected string
// is "<integer>[,<integer>]".
bool parseMemoryOption(const StringRef &arg, raw_ostream &diagnostics,
                       uint64_t &reserve, uint64_t &commit) {
  StringRef reserveStr, commitStr;
  llvm::tie(reserveStr, commitStr) = arg.split(',');
  if (!checkNumber(reserveStr, "invalid size: ", diagnostics))
    return false;
  reserve = atoi(reserveStr.str().c_str());
  if (!commitStr.empty()) {
    if (!checkNumber(commitStr, "invalid size: ", diagnostics))
      return false;
    commit = atoi(commitStr.str().c_str());
  }
  return true;
}

// Parse /base command line option. The argument for the parameter is in the
// form of "<address>[:<size>]".
bool parseBaseOption(PECOFFLinkingContext &context, const StringRef &arg,
                     raw_ostream &diagnostics) {
  // Size should be set to SizeOfImage field in the COFF header, and if it's
  // smaller than the actual size, the linker should warn about that. Currently
  // we just ignore the value of size parameter.
  uint64_t addr, size;
  if (!parseMemoryOption(arg, diagnostics, addr, size))
    return false;
  // It's an error if the base address is not multiple of 64K.
  if (addr & 0xffff) {
    diagnostics << "Base address have to be multiple of 64K, but got "
                << addr << "\n";
    return false;
  }
  context.setBaseAddress(addr);
  return true;
}

// Parse /stack command line option
bool parseStackOption(PECOFFLinkingContext &context, const StringRef &arg,
                      raw_ostream &diagnostics) {
  uint64_t reserve;
  uint64_t commit = context.getStackCommit();
  if (!parseMemoryOption(arg, diagnostics, reserve, commit))
    return false;
  context.setStackReserve(reserve);
  context.setStackCommit(commit);
  return true;
}

// Parse /heap command line option.
bool parseHeapOption(PECOFFLinkingContext &context, const StringRef &arg,
                     raw_ostream &diagnostics) {
  uint64_t reserve;
  uint64_t commit = context.getHeapCommit();
  if (!parseMemoryOption(arg, diagnostics, reserve, commit))
    return false;
  context.setHeapReserve(reserve);
  context.setHeapCommit(commit);
  return true;
}

// Returns subsystem type for the given string.
llvm::COFF::WindowsSubsystem stringToWinSubsystem(StringRef str) {
  std::string arg(str.lower());
  return llvm::StringSwitch<llvm::COFF::WindowsSubsystem>(arg)
      .Case("windows", llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_GUI)
      .Case("console", llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI)
      .Default(llvm::COFF::IMAGE_SUBSYSTEM_UNKNOWN);
}

bool parseMinOSVersion(PECOFFLinkingContext &context,
                       const StringRef &osVersion, raw_ostream &diagnostics) {
  StringRef majorVersion, minorVersion;
  llvm::tie(majorVersion, minorVersion) = osVersion.split('.');
  if (minorVersion.empty())
    minorVersion = "0";
  if (!checkNumber(majorVersion, "invalid OS major version: ", diagnostics))
    return false;
  if (!checkNumber(minorVersion, "invalid OS minor version: ", diagnostics))
    return false;
  PECOFFLinkingContext::OSVersion minOSVersion(
      atoi(majorVersion.str().c_str()), atoi(minorVersion.str().c_str()));
  context.setMinOSVersion(minOSVersion);
  return true;
}

// Parse /subsystem command line option. The form of /subsystem is
// "subsystem_name[,majorOSVersion[.minorOSVersion]]".
bool parseSubsystemOption(PECOFFLinkingContext &context, std::string arg,
                          raw_ostream &diagnostics) {
  StringRef subsystemStr, osVersionStr;
  llvm::tie(subsystemStr, osVersionStr) = StringRef(arg).split(',');

  // Parse optional OS version if exists.
  if (!osVersionStr.empty())
    if (!parseMinOSVersion(context, osVersionStr, diagnostics))
      return false;

  // Parse subsystem name.
  llvm::COFF::WindowsSubsystem subsystem = stringToWinSubsystem(subsystemStr);
  if (subsystem == llvm::COFF::IMAGE_SUBSYSTEM_UNKNOWN) {
    diagnostics << "error: unknown subsystem name: " << subsystemStr << "\n";
    return false;
  }
  context.setSubsystem(subsystem);
  return true;
}

// Replace a file extension with ".exe". If the given file has no
// extension, just add ".exe".
StringRef getDefaultOutputFileName(PECOFFLinkingContext &context,
                                   StringRef path) {
  SmallString<128> smallStr = path;
  llvm::sys::path::replace_extension(smallStr, ".exe");
  return context.allocateString(smallStr.str());
}

// Split the given string with spaces.
std::vector<std::string> splitArgList(std::string str) {
  std::stringstream stream(str);
  std::istream_iterator<std::string> begin(stream);
  std::istream_iterator<std::string> end;
  return std::vector<std::string>(begin, end);
}

// Split the given string with the path separator.
std::vector<StringRef> splitPathList(StringRef str) {
  std::vector<StringRef> ret;
  while (!str.empty()) {
    StringRef path;
    llvm::tie(path, str) = str.split(';');
    ret.push_back(path);
  }
  return std::move(ret);
}

// Handle /failifmatch option.
bool handleFailIfMismatchOption(StringRef option,
                                std::map<StringRef, StringRef> &mustMatch,
                                raw_ostream &diagnostics) {
  StringRef key, value;
  llvm::tie(key, value) = option.split('=');
  if (key.empty() || value.empty()) {
    diagnostics << "error: malformed /failifmatch option: " << option << "\n";
    return false;
  }
  auto it = mustMatch.find(key);
  if (it != mustMatch.end() && it->second != value) {
    diagnostics << "error: mismatch detected: '" << it->second << "' and '"
                << value << "' for key '" << key << "'\n";
    return false;
  }
  mustMatch[key] = value;
  return true;
}

// Add ".lib" extension if the path does not already have the extension to mimic
// link.exe behavior.
StringRef canonicalizeImportLibraryPath(PECOFFLinkingContext &context,
                                        StringRef path) {
  std::string s(path.lower());
  if (StringRef(s).endswith(".lib"))
    return path;
  return context.allocateString(std::string(path).append(".lib"));
}

// Process "LINK" environment variable. If defined, the value of the variable
// should be processed as command line arguments.
std::vector<const char *> processLinkEnv(PECOFFLinkingContext &context,
                                         int argc, const char **argv) {
  std::vector<const char *> ret;
  // The first argument is the name of the command. This should stay at the head
  // of the argument list.
  assert(argc > 0);
  ret.push_back(argv[0]);

  // Add arguments specified by the LINK environment variable.
  if (char *envp = ::getenv("LINK"))
    for (std::string &arg : splitArgList(envp))
      ret.push_back(context.allocateString(arg).data());

  // Add the rest of arguments passed via the command line.
  for (int i = 1; i < argc; ++i)
    ret.push_back(argv[i]);
  ret.push_back(nullptr);
  return std::move(ret);
}

// Process "LIB" environment variable. The variable contains a list of search
// paths separated by semicolons.
void processLibEnv(PECOFFLinkingContext &context) {
  if (char *envp = ::getenv("LIB"))
    for (StringRef path : splitPathList(envp))
      context.appendInputSearchPath(context.allocateString(path));
}

// Parses the given command line options and returns the result. Returns NULL if
// there's an error in the options.
std::unique_ptr<llvm::opt::InputArgList> parseArgs(int argc, const char *argv[],
                                                   raw_ostream &diagnostics) {
  // Parse command line options using WinLinkOptions.td
  std::unique_ptr<llvm::opt::InputArgList> parsedArgs;
  WinLinkOptTable table;
  unsigned missingIndex;
  unsigned missingCount;
  parsedArgs.reset(table.ParseArgs(&argv[1], &argv[argc], missingIndex, missingCount));
  if (missingCount) {
    diagnostics << "error: missing arg value for '"
                << parsedArgs->getArgString(missingIndex) << "' expected "
                << missingCount << " argument(s).\n";
    return nullptr;
  }

  // Show warning for unknown arguments
  for (auto it = parsedArgs->filtered_begin(OPT_UNKNOWN),
            ie = parsedArgs->filtered_end(); it != ie; ++it) {
    diagnostics << "warning: ignoring unknown argument: "
                << (*it)->getAsString(*parsedArgs) << "\n";
  }

  return parsedArgs;
}

} // namespace


bool WinLinkDriver::linkPECOFF(int argc, const char *argv[],
                               raw_ostream &diagnostics) {
  PECOFFLinkingContext context;
  std::vector<const char *> newargv = processLinkEnv(context, argc, argv);
  processLibEnv(context);
  if (parse(newargv.size() - 1, &newargv[0], context, diagnostics))
    return true;
  return link(context, diagnostics);
}

bool WinLinkDriver::parse(int argc, const char *argv[],
                          PECOFFLinkingContext &context,
                          raw_ostream &diagnostics) {
  // Parse the options.
  std::unique_ptr<llvm::opt::InputArgList> parsedArgs = parseArgs(
      argc, argv, diagnostics);
  if (!parsedArgs)
    return true;

  // handle /help
  if (parsedArgs->getLastArg(OPT_help)) {
    WinLinkOptTable table;
    table.PrintHelp(llvm::outs(), argv[0], "LLVM Linker", false);
    return true;
  }

  // Copy -mllvm
  for (llvm::opt::arg_iterator it = parsedArgs->filtered_begin(OPT_mllvm),
                               ie = parsedArgs->filtered_end();
       it != ie; ++it) {
    context.appendLLVMOption((*it)->getValue());
  }

  // handle /base
  if (llvm::opt::Arg *arg = parsedArgs->getLastArg(OPT_base))
    if (!parseBaseOption(context, arg->getValue(), diagnostics))
      return true;

  // handle /stack
  if (llvm::opt::Arg *arg = parsedArgs->getLastArg(OPT_stack))
    if (!parseStackOption(context, arg->getValue(), diagnostics))
      return true;

  // handle /heap
  if (llvm::opt::Arg *arg = parsedArgs->getLastArg(OPT_heap))
    if (!parseHeapOption(context, arg->getValue(), diagnostics))
      return true;

  // handle /subsystem
  if (llvm::opt::Arg *arg = parsedArgs->getLastArg(OPT_subsystem))
    if (!parseSubsystemOption(context, arg->getValue(), diagnostics))
      return true;

  // handle /entry
  if (llvm::opt::Arg *arg = parsedArgs->getLastArg(OPT_entry))
    context.setEntrySymbolName(arg->getValue());

  // handle /libpath
  for (llvm::opt::arg_iterator it = parsedArgs->filtered_begin(OPT_libpath),
                               ie = parsedArgs->filtered_end();
       it != ie; ++it) {
    context.appendInputSearchPath((*it)->getValue());
  }

  // handle /force
  if (parsedArgs->getLastArg(OPT_force))
    context.setAllowRemainingUndefines(true);

  // handle /nxcompat:no
  if (parsedArgs->getLastArg(OPT_no_nxcompat))
    context.setNxCompat(false);

  // handle /largeaddressaware
  if (parsedArgs->getLastArg(OPT_largeaddressaware))
    context.setLargeAddressAware(true);

  // handle /fixed
  if (parsedArgs->getLastArg(OPT_fixed))
    context.setBaseRelocationEnabled(false);

  // handle /tsaware:no
  if (parsedArgs->getLastArg(OPT_no_tsaware))
    context.setTerminalServerAware(false);

  // handle /include
  for (llvm::opt::arg_iterator it = parsedArgs->filtered_begin(OPT_incl),
                               ie = parsedArgs->filtered_end();
       it != ie; ++it) {
    context.addInitialUndefinedSymbol((*it)->getValue());
  }

  // handle /out
  if (llvm::opt::Arg *outpath = parsedArgs->getLastArg(OPT_out))
    context.setOutputPath(outpath->getValue());

  // handle /defaultlib
  std::vector<StringRef> defaultLibs;
  for (llvm::opt::arg_iterator it = parsedArgs->filtered_begin(OPT_defaultlib),
                               ie = parsedArgs->filtered_end();
       it != ie; ++it) {
    defaultLibs.push_back((*it)->getValue());
  }

  // Handle /failifmismatch. /failifmismatch is the hidden linker option behind
  // the scenes of "detect_mismatch" pragma. If the compiler finds "#pragma
  // detect_mismatch(name, value)", it outputs "/failifmismatch:name=value" to
  // the .drectve section of the resultant object file. The linker raises an
  // error if conflicting /failmismatch options are given. Conflicting options
  // are the options with the same key but with different values.
  //
  // This feature is used to prevent inconsistent object files from linking.
  std::map<StringRef, StringRef> mustMatch;
  for (llvm::opt::arg_iterator
           it = parsedArgs->filtered_begin(OPT_failifmismatch),
           ie = parsedArgs->filtered_end();
       it != ie; ++it) {
    if (!handleFailIfMismatchOption((*it)->getValue(), mustMatch, diagnostics))
      return true;
  }

  // Add input files
  std::vector<StringRef> inputPaths;
  for (llvm::opt::arg_iterator it = parsedArgs->filtered_begin(OPT_INPUT),
                               ie = parsedArgs->filtered_end();
       it != ie; ++it) {
    inputPaths.push_back((*it)->getValue());
  }

  // Add input files specified via the command line.
  for (const StringRef path : inputPaths)
    context.appendInputFileOrLibrary(path);

  // Add the library files specified by /defaultlib option. The files
  // specified by the option should have lower precedence than the other files
  // added above, which is important for link.exe compatibility.
  for (const StringRef path : defaultLibs)
    context.appendLibraryFile(canonicalizeImportLibraryPath(context, path));

  // If /out option was not specified, the default output file name is
  // constructed by replacing an extension of the first input file
  // with ".exe".
  if (context.outputPath().empty() && !inputPaths.empty())
    context.setOutputPath(getDefaultOutputFileName(context, inputPaths[0]));

  // Validate the combination of options used.
  return context.validate(diagnostics);
}

} // namespace lld
