//===- lib/Driver/GnuLdDriver.cpp -----------------------------------------===//
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
/// Concrete instance of the Driver for GNU's ld.
///
//===----------------------------------------------------------------------===//

#include "lld/Driver/Driver.h"
#include "lld/ReaderWriter/ELFLinkingContext.h"
#include "lld/ReaderWriter/ELFTargets.h"
#include "lld/ReaderWriter/LinkerScript.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"
#include <cstring>
#include <tuple>

using namespace lld;

using llvm::BumpPtrAllocator;

namespace {

// Create enum with OPT_xxx values for each option in GnuLdOptions.td
enum {
  OPT_INVALID = 0,
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELP, META) \
          OPT_##ID,
#include "GnuLdOptions.inc"
#undef OPTION
};

// Create prefix string literals used in GnuLdOptions.td
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "GnuLdOptions.inc"
#undef PREFIX

// Create table mapping all options defined in GnuLdOptions.td
static const llvm::opt::OptTable::Info infoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELPTEXT, METAVAR)   \
  { PREFIX, NAME, HELPTEXT, METAVAR, OPT_##ID, llvm::opt::Option::KIND##Class, \
    PARAM, FLAGS, OPT_##GROUP, OPT_##ALIAS, ALIASARGS },
#include "GnuLdOptions.inc"
#undef OPTION
};


// Create OptTable class for parsing actual command line arguments
class GnuLdOptTable : public llvm::opt::OptTable {
public:
  GnuLdOptTable() : OptTable(infoTable, llvm::array_lengthof(infoTable)){}
};

class DriverStringSaver : public llvm::cl::StringSaver {
public:
  DriverStringSaver(BumpPtrAllocator &alloc) : _alloc(alloc) {}

  const char *SaveString(const char *s) override {
    char *p = _alloc.Allocate<char>(strlen(s) + 1);
    strcpy(p, s);
    return p;
  }

private:
  BumpPtrAllocator &_alloc;
};

} // anonymous namespace

// If a command line option starts with "@", the driver reads its suffix as a
// file, parse its contents as a list of command line options, and insert them
// at the original @file position. If file cannot be read, @file is not expanded
// and left unmodified. @file can appear in a response file, so it's a recursive
// process.
static std::tuple<int, const char **>
maybeExpandResponseFiles(int argc, const char **argv, BumpPtrAllocator &alloc) {
  // Expand response files.
  SmallVector<const char *, 256> smallvec;
  for (int i = 0; i < argc; ++i)
    smallvec.push_back(argv[i]);
  DriverStringSaver saver(alloc);
  llvm::cl::ExpandResponseFiles(saver, llvm::cl::TokenizeGNUCommandLine, smallvec);

  // Pack the results to a C-array and return it.
  argc = smallvec.size();
  const char **copy = alloc.Allocate<const char *>(argc + 1);
  std::copy(smallvec.begin(), smallvec.end(), copy);
  copy[argc] = nullptr;
  return std::make_tuple(argc, copy);
}

static std::error_code
getFileMagic(StringRef path, llvm::sys::fs::file_magic &magic) {
  std::error_code ec = llvm::sys::fs::identify_magic(path, magic);
  if (ec)
    return ec;
  switch (magic) {
  case llvm::sys::fs::file_magic::archive:
  case llvm::sys::fs::file_magic::elf_relocatable:
  case llvm::sys::fs::file_magic::elf_shared_object:
  case llvm::sys::fs::file_magic::unknown:
    return std::error_code();
  default:
    return make_dynamic_error_code(StringRef("unknown type of object file"));
  }
}

// Parses an argument of --defsym=<sym>=<number>
static bool parseDefsymAsAbsolute(StringRef opt, StringRef &sym,
                                  uint64_t &addr) {
  size_t equalPos = opt.find('=');
  if (equalPos == 0 || equalPos == StringRef::npos)
    return false;
  sym = opt.substr(0, equalPos);
  if (opt.substr(equalPos + 1).getAsInteger(0, addr))
    return false;
  return true;
}

// Parses an argument of --defsym=<sym>=<sym>
static bool parseDefsymAsAlias(StringRef opt, StringRef &sym,
                               StringRef &target) {
  size_t equalPos = opt.find('=');
  if (equalPos == 0 || equalPos == StringRef::npos)
    return false;
  sym = opt.substr(0, equalPos);
  target = opt.substr(equalPos + 1);
  return !target.empty();
}

// Parses dashz options for max-page-size.
static bool parseZOption(StringRef opt, uint64_t &val) {
  size_t equalPos = opt.find('=');
  if (equalPos == 0 || equalPos == StringRef::npos)
    return false;
  StringRef value = opt.substr(equalPos + 1);
  val = 0;
  if (value.getAsInteger(0, val) || !val)
    return false;
  return true;
}

bool GnuLdDriver::linkELF(int argc, const char *argv[],
                          raw_ostream &diagnostics) {
  BumpPtrAllocator alloc;
  std::tie(argc, argv) = maybeExpandResponseFiles(argc, argv, alloc);
  std::unique_ptr<ELFLinkingContext> options;
  if (!parse(argc, argv, options, diagnostics))
    return false;
  if (!options)
    return true;
  return link(*options, diagnostics);
}

static llvm::Optional<llvm::Triple::ArchType>
getArchType(const llvm::Triple &triple, StringRef value) {
  switch (triple.getArch()) {
  case llvm::Triple::x86:
  case llvm::Triple::x86_64:
    if (value == "elf_i386")
      return llvm::Triple::x86;
    if (value == "elf_x86_64")
      return llvm::Triple::x86_64;
    return llvm::None;
  case llvm::Triple::mipsel:
    if (value == "elf32ltsmip")
      return llvm::Triple::mipsel;
    return llvm::None;
  case llvm::Triple::aarch64:
    if (value == "aarch64linux")
      return llvm::Triple::aarch64;
    return llvm::None;
  case llvm::Triple::arm:
    if (value == "armelf_linux_eabi")
      return llvm::Triple::arm;
    return llvm::None;
  default:
    return llvm::None;
  }
}

static bool isLinkerScript(StringRef path, raw_ostream &diag) {
  llvm::sys::fs::file_magic magic = llvm::sys::fs::file_magic::unknown;
  std::error_code ec = getFileMagic(path, magic);
  if (ec) {
    diag << "unknown input file format for file " << path << "\n";
    return false;
  }
  return magic == llvm::sys::fs::file_magic::unknown;
}

static ErrorOr<StringRef>
findFile(ELFLinkingContext &ctx, StringRef path, bool dashL) {
  // If the path was referred to by using a -l argument, let's search
  // for the file in the search path.
  if (dashL) {
    ErrorOr<StringRef> pathOrErr = ctx.searchLibrary(path);
    if (std::error_code ec = pathOrErr.getError())
      return make_dynamic_error_code(
          Twine("Unable to find library -l") + path + ": " + ec.message());
    path = pathOrErr.get();
  }
  if (!llvm::sys::fs::exists(path))
    return make_dynamic_error_code(
        Twine("lld: cannot find file ") + path);
  return path;
}

static bool isPathUnderSysroot(StringRef sysroot, StringRef path) {
  if (sysroot.empty())
    return false;
  while (!path.empty() && !llvm::sys::fs::equivalent(sysroot, path))
    path = llvm::sys::path::parent_path(path);
  return !path.empty();
}

static std::error_code
evaluateLinkerScriptGroup(ELFLinkingContext &ctx, StringRef path,
                          const script::Group *group, raw_ostream &diag) {
  bool sysroot = (!ctx.getSysroot().empty()
                  && isPathUnderSysroot(ctx.getSysroot(), path));
  int numfiles = 0;
  for (const script::Path &path : group->getPaths()) {
    ErrorOr<StringRef> pathOrErr = path._isDashlPrefix
      ? ctx.searchLibrary(path._path) : ctx.searchFile(path._path, sysroot);
    if (std::error_code ec = pathOrErr.getError())
      return make_dynamic_error_code(
        Twine("Unable to find file ") + path._path + ": " + ec.message());

    std::vector<std::unique_ptr<File>> files
      = loadFile(ctx, pathOrErr.get(), false);
    for (std::unique_ptr<File> &file : files) {
      if (ctx.logInputFiles())
        diag << file->path() << "\n";
      ctx.getNodes().push_back(llvm::make_unique<FileNode>(std::move(file)));
      ++numfiles;
    }
  }
  ctx.getNodes().push_back(llvm::make_unique<GroupEnd>(numfiles));
  return std::error_code();
}

static std::error_code
evaluateLinkerScript(ELFLinkingContext &ctx, StringRef path,
                     raw_ostream &diag) {
  // Read the script file from disk and parse.
  ErrorOr<std::unique_ptr<MemoryBuffer>> mb =
      MemoryBuffer::getFileOrSTDIN(path);
  if (std::error_code ec = mb.getError())
    return ec;
  auto lexer = llvm::make_unique<script::Lexer>(std::move(mb.get()));
  auto parser = llvm::make_unique<script::Parser>(*lexer);
  script::LinkerScript *script = parser->parse();
  if (!script)
    return LinkerScriptReaderError::parse_error;

  // Evaluate script commands.
  // Currently we only recognize GROUP() command.
  for (const script::Command *c : script->_commands)
    if (auto *group = dyn_cast<script::Group>(c))
      if (std::error_code ec = evaluateLinkerScriptGroup(ctx, path, group, diag))
        return ec;
  return std::error_code();
}

bool GnuLdDriver::applyEmulation(llvm::Triple &triple,
                                 llvm::opt::InputArgList &args,
                                 raw_ostream &diagnostics) {
  llvm::opt::Arg *arg = args.getLastArg(OPT_m);
  if (!arg)
    return true;
  llvm::Optional<llvm::Triple::ArchType> arch =
      getArchType(triple, arg->getValue());
  if (!arch) {
    diagnostics << "error: unsupported emulation '" << arg->getValue()
                << "'.\n";
    return false;
  }
  triple.setArch(*arch);
  return true;
}

void GnuLdDriver::addPlatformSearchDirs(ELFLinkingContext &ctx,
                                       llvm::Triple &triple,
                                       llvm::Triple &baseTriple) {
  if (triple.getOS() == llvm::Triple::NetBSD &&
      triple.getArch() == llvm::Triple::x86 &&
      baseTriple.getArch() == llvm::Triple::x86_64) {
    ctx.addSearchPath("=/usr/lib/i386");
    return;
  }
  ctx.addSearchPath("=/usr/lib");
}

std::unique_ptr<ELFLinkingContext>
createELFLinkingContext(llvm::Triple triple) {
  switch (triple.getArch()) {
  case llvm::Triple::x86:
    return std::unique_ptr<ELFLinkingContext>(
        new lld::elf::X86LinkingContext(triple));
  case llvm::Triple::x86_64:
    return std::unique_ptr<ELFLinkingContext>(
        new lld::elf::X86_64LinkingContext(triple));
  case llvm::Triple::hexagon:
    return std::unique_ptr<ELFLinkingContext>(
        new lld::elf::HexagonLinkingContext(triple));
  case llvm::Triple::mipsel:
    return std::unique_ptr<ELFLinkingContext>(
        new lld::elf::MipsLinkingContext(triple));
  case llvm::Triple::ppc:
    return std::unique_ptr<ELFLinkingContext>(
        new lld::elf::PPCLinkingContext(triple));
  case llvm::Triple::aarch64:
    return std::unique_ptr<ELFLinkingContext>(
        new lld::elf::AArch64LinkingContext(triple));
  case llvm::Triple::arm:
    return std::unique_ptr<ELFLinkingContext>(
        new lld::elf::ARMLinkingContext(triple));
  default:
    return nullptr;
  }
}

bool GnuLdDriver::parse(int argc, const char *argv[],
                        std::unique_ptr<ELFLinkingContext> &context,
                        raw_ostream &diagnostics) {
  // Parse command line options using GnuLdOptions.td
  std::unique_ptr<llvm::opt::InputArgList> parsedArgs;
  GnuLdOptTable table;
  unsigned missingIndex;
  unsigned missingCount;

  parsedArgs.reset(
      table.ParseArgs(&argv[1], &argv[argc], missingIndex, missingCount));
  if (missingCount) {
    diagnostics << "error: missing arg value for '"
                << parsedArgs->getArgString(missingIndex) << "' expected "
                << missingCount << " argument(s).\n";
    return false;
  }

  // Handle --help
  if (parsedArgs->getLastArg(OPT_help)) {
    table.PrintHelp(llvm::outs(), argv[0], "LLVM Linker", false);
    return true;
  }

  // Use -target or use default target triple to instantiate LinkingContext
  llvm::Triple baseTriple;
  if (llvm::opt::Arg *trip = parsedArgs->getLastArg(OPT_target))
    baseTriple = llvm::Triple(trip->getValue());
  else
    baseTriple = getDefaultTarget(argv[0]);
  llvm::Triple triple(baseTriple);

  if (!applyEmulation(triple, *parsedArgs, diagnostics))
    return false;

  std::unique_ptr<ELFLinkingContext> ctx(createELFLinkingContext(triple));

  if (!ctx) {
    diagnostics << "unknown target triple\n";
    return false;
  }

  std::stack<int> groupStack;
  int numfiles = 0;

  bool asNeeded = false;
  bool wholeArchive = false;

  bool _outputOptionSet = false;

  // Ignore unknown arguments.
  for (auto unknownArg : parsedArgs->filtered(OPT_UNKNOWN))
    diagnostics << "warning: ignoring unknown argument: "
                << unknownArg->getValue() << "\n";

  // Set sys root path.
  if (llvm::opt::Arg *sysRootPath = parsedArgs->getLastArg(OPT_sysroot))
    ctx->setSysroot(sysRootPath->getValue());

  // Add all search paths.
  for (auto libDir : parsedArgs->filtered(OPT_L))
    ctx->addSearchPath(libDir->getValue());

  if (!parsedArgs->hasArg(OPT_nostdlib))
    addPlatformSearchDirs(*ctx, triple, baseTriple);

  // Handle --demangle option(For compatibility)
  if (parsedArgs->getLastArg(OPT_demangle))
    ctx->setDemangleSymbols(true);

  // Handle --no-demangle option.
  if (parsedArgs->getLastArg(OPT_no_demangle))
    ctx->setDemangleSymbols(false);

  // Figure out output kind ( -r, -static, -shared)
  if (llvm::opt::Arg *kind =
          parsedArgs->getLastArg(OPT_relocatable, OPT_static, OPT_shared,
                                 OPT_nmagic, OPT_omagic, OPT_no_omagic)) {
    switch (kind->getOption().getID()) {
    case OPT_relocatable:
      ctx->setOutputELFType(llvm::ELF::ET_REL);
      ctx->setPrintRemainingUndefines(false);
      ctx->setAllowRemainingUndefines(true);
      break;
    case OPT_static:
      ctx->setOutputELFType(llvm::ELF::ET_EXEC);
      ctx->setIsStaticExecutable(true);
      break;
    case OPT_shared:
      ctx->setOutputELFType(llvm::ELF::ET_DYN);
      ctx->setAllowShlibUndefines(true);
      ctx->setUseShlibUndefines(false);
      ctx->setPrintRemainingUndefines(false);
      ctx->setAllowRemainingUndefines(true);
      break;
    }
  }

  // Figure out if the output type is nmagic/omagic
  if (llvm::opt::Arg *kind =
          parsedArgs->getLastArg(OPT_nmagic, OPT_omagic, OPT_no_omagic)) {
    switch (kind->getOption().getID()) {
    case OPT_nmagic:
      ctx->setOutputMagic(ELFLinkingContext::OutputMagic::NMAGIC);
      ctx->setIsStaticExecutable(true);
      break;

    case OPT_omagic:
      ctx->setOutputMagic(ELFLinkingContext::OutputMagic::OMAGIC);
      ctx->setIsStaticExecutable(true);
      break;

    case OPT_no_omagic:
      ctx->setOutputMagic(ELFLinkingContext::OutputMagic::DEFAULT);
      ctx->setNoAllowDynamicLibraries();
      break;
    }
  }

  for (auto inputArg : *parsedArgs) {
    switch (inputArg->getOption().getID()) {
    case OPT_merge_strings:
      ctx->setMergeCommonStrings(true);
      break;
    case OPT_t:
      ctx->setLogInputFiles(true);
      break;
    case OPT_use_shlib_undefs:
      ctx->setUseShlibUndefines(true);
      break;
    case OPT_no_allow_shlib_undefs:
      ctx->setAllowShlibUndefines(false);
      break;
    case OPT_allow_shlib_undefs:
      ctx->setAllowShlibUndefines(true);
      break;
    }
  }

  // Register possible input file parsers.
  ctx->registry().addSupportELFObjects(
      ctx->mergeCommonStrings(),
      ctx->targetHandler());
  ctx->registry().addSupportArchives(ctx->logInputFiles());
  ctx->registry().addSupportYamlFiles();
  ctx->registry().addSupportNativeObjects();
  if (ctx->allowLinkWithDynamicLibraries())
    ctx->registry().addSupportELFDynamicSharedObjects(
        ctx->useShlibUndefines(), ctx->targetHandler());

  // Process all the arguments and create input files.
  for (auto inputArg : *parsedArgs) {
    switch (inputArg->getOption().getID()) {
    case OPT_mllvm:
      ctx->appendLLVMOption(inputArg->getValue());
      break;
    case OPT_e:
      ctx->setEntrySymbolName(inputArg->getValue());
      break;

    case OPT_output:
      _outputOptionSet = true;
      ctx->setOutputPath(inputArg->getValue());
      break;

    case OPT_noinhibit_exec:
      ctx->setAllowRemainingUndefines(true);
      break;

    case OPT_export_dynamic:
      ctx->setExportDynamic(true);
      break;

    case OPT_allow_multiple_definition:
      ctx->setAllowDuplicates(true);
      break;

    case OPT_dynamic_linker:
      ctx->setInterpreter(inputArg->getValue());
      break;

    case OPT_u:
      ctx->addInitialUndefinedSymbol(inputArg->getValue());
      break;

    case OPT_init:
      ctx->setInitFunction(inputArg->getValue());
      break;

    case OPT_fini:
      ctx->setFiniFunction(inputArg->getValue());
      break;

    case OPT_output_filetype:
      ctx->setOutputFileType(inputArg->getValue());
      break;

    case OPT_no_whole_archive:
      wholeArchive = false;
      break;

    case OPT_whole_archive:
      wholeArchive = true;
      break;

    case OPT_as_needed:
      asNeeded = true;
      break;

    case OPT_no_as_needed:
      asNeeded = false;
      break;

    case OPT_defsym: {
      StringRef sym, target;
      uint64_t addr;
      if (parseDefsymAsAbsolute(inputArg->getValue(), sym, addr)) {
        ctx->addInitialAbsoluteSymbol(sym, addr);
      } else if (parseDefsymAsAlias(inputArg->getValue(), sym, target)) {
        ctx->addAlias(sym, target);
      } else {
        diagnostics << "invalid --defsym: " << inputArg->getValue() << "\n";
        return false;
      }
      break;
    }

    case OPT_start_group:
      groupStack.push(numfiles);
      break;

    case OPT_end_group: {
      if (groupStack.empty()) {
        diagnostics << "stray --end-group\n";
        return false;
      }
      int startGroupPos = groupStack.top();
      ctx->getNodes().push_back(
          llvm::make_unique<GroupEnd>(numfiles - startGroupPos));
      groupStack.pop();
      break;
    }

    case OPT_z: {
      StringRef extOpt = inputArg->getValue();
      if (extOpt == "muldefs")
        ctx->setAllowDuplicates(true);
      else if (extOpt.startswith("max-page-size")) {
        // Parse -z max-page-size option.
        // The default page size is considered the minimum page size the user
        // can set, check the user input if its atleast the minimum page size
        // and doesnot exceed the maximum page size allowed for the target.
        uint64_t maxPageSize = 0;

        // Error if the page size user set is less than the maximum page size
        // and greather than the default page size and the user page size is a
        // modulo of the default page size.
        if ((!parseZOption(extOpt, maxPageSize)) ||
            (maxPageSize < ctx->getPageSize()) ||
            (maxPageSize % ctx->getPageSize())) {
          diagnostics << "invalid option: " << extOpt << "\n";
          return false;
        }
        ctx->setMaxPageSize(maxPageSize);
      } else
        diagnostics << "warning: ignoring unknown argument for -z: " << extOpt
                    << "\n";
      break;
    }

    case OPT_INPUT:
    case OPT_l: {
      bool dashL = (inputArg->getOption().getID() == OPT_l);
      StringRef path = inputArg->getValue();

      ErrorOr<StringRef> pathOrErr = findFile(*ctx, path, dashL);
      if (std::error_code ec = pathOrErr.getError()) {
        auto file = llvm::make_unique<ErrorFile>(path, ec);
        ctx->getNodes().push_back(llvm::make_unique<FileNode>(std::move(file)));
        break;
      }
      StringRef realpath = pathOrErr.get();

      bool isScript =
          (!path.endswith(".objtxt") && isLinkerScript(realpath, diagnostics));
      if (isScript) {
        if (ctx->logInputFiles())
          diagnostics << path << "\n";
        std::error_code ec = evaluateLinkerScript(*ctx, realpath, diagnostics);
        if (ec) {
          diagnostics << path << ": Error parsing linker script: "
                      << ec.message() << "\n";
          return false;
        }
        break;
      }
      std::vector<std::unique_ptr<File>> files
          = loadFile(*ctx, realpath, wholeArchive);
      for (std::unique_ptr<File> &file : files) {
        if (ctx->logInputFiles())
          diagnostics << file->path() << "\n";
        auto node = llvm::make_unique<FileNode>(std::move(file));
        node->setAsNeeded(asNeeded);
        ctx->getNodes().push_back(std::move(node));
      }
      numfiles += files.size();
      break;
    }

    case OPT_rpath: {
      SmallVector<StringRef, 2> rpaths;
      StringRef(inputArg->getValue()).split(rpaths, ":");
      for (auto path : rpaths)
        ctx->addRpath(path);
      break;
    }

    case OPT_rpath_link: {
      SmallVector<StringRef, 2> rpaths;
      StringRef(inputArg->getValue()).split(rpaths, ":");
      for (auto path : rpaths)
        ctx->addRpathLink(path);
      break;
    }

    case OPT_soname:
      ctx->setSharedObjectName(inputArg->getValue());
      break;

    case OPT_rosegment:
      ctx->setCreateSeparateROSegment();
      break;

    case OPT_no_align_segments:
      ctx->setAlignSegments(false);
      break;

    case OPT_image_base: {
      uint64_t baseAddress = 0;
      StringRef inputValue = inputArg->getValue();
      if ((inputValue.getAsInteger(0, baseAddress)) || !baseAddress) {
        diagnostics << "invalid value for image base " << inputValue
                    << "\n";
        return false;
      }
      ctx->setBaseAddress(baseAddress);
      break;
    }

    default:
      break;
    } // end switch on option ID
  }   // end for

  if (ctx->getNodes().empty()) {
    diagnostics << "No input files\n";
    return false;
  }

  // Set default output file name if the output file was not
  // specified.
  if (!_outputOptionSet) {
    switch (ctx->outputFileType()) {
    case LinkingContext::OutputFileType::YAML:
      ctx->setOutputPath("-");
      break;
    case LinkingContext::OutputFileType::Native:
      ctx->setOutputPath("a.native");
      break;
    default:
      ctx->setOutputPath("a.out");
      break;
    }
  }

  // Validate the combination of options used.
  if (!ctx->validate(diagnostics))
    return false;

  context.swap(ctx);
  return true;
}

/// Get the default target triple based on either the program name
/// (e.g. "x86-ibm-linux-lld") or the primary target llvm was configured for.
llvm::Triple GnuLdDriver::getDefaultTarget(const char *progName) {
  SmallVector<StringRef, 4> components;
  llvm::SplitString(llvm::sys::path::stem(progName), components, "-");
  // If has enough parts to be start with a triple.
  if (components.size() >= 4) {
    llvm::Triple triple(components[0], components[1], components[2],
                        components[3]);
    // If first component looks like an arch.
    if (triple.getArch() != llvm::Triple::UnknownArch)
      return triple;
  }

  // Fallback to use whatever default triple llvm was configured for.
  return llvm::Triple(llvm::sys::getDefaultTargetTriple());
}
