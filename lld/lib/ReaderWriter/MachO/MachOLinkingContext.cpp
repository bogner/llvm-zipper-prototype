//===- lib/ReaderWriter/MachO/MachOLinkingContext.cpp ---------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/ReaderWriter/MachOLinkingContext.h"

#include "ArchHandler.h"
#include "MachOPasses.h"

#include "lld/Core/PassManager.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"
#include "lld/Passes/LayoutPass.h"
#include "lld/Passes/RoundTripYAMLPass.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MachO.h"
#include "llvm/Support/Path.h"

using lld::mach_o::ArchHandler;
using namespace llvm::MachO;

namespace lld {

bool MachOLinkingContext::parsePackedVersion(StringRef str, uint32_t &result) {
  result = 0;

  if (str.empty())
    return false;

  SmallVector<StringRef, 3> parts;
  llvm::SplitString(str, parts, ".");

  unsigned long long num;
  if (llvm::getAsUnsignedInteger(parts[0], 10, num))
    return true;
  if (num > 65535)
    return true;
  result = num << 16;

  if (parts.size() > 1) {
    if (llvm::getAsUnsignedInteger(parts[1], 10, num))
      return true;
    if (num > 255)
      return true;
    result |= (num << 8);
  }

  if (parts.size() > 2) {
    if (llvm::getAsUnsignedInteger(parts[2], 10, num))
      return true;
    if (num > 255)
      return true;
    result |= num;
  }

  return false;
}


MachOLinkingContext::ArchInfo MachOLinkingContext::_s_archInfos[] = {
  { "x86_64", arch_x86_64, true,  CPU_TYPE_X86_64,  CPU_SUBTYPE_X86_64_ALL },
  { "i386",   arch_x86,    true,  CPU_TYPE_I386,    CPU_SUBTYPE_X86_ALL },
  { "ppc",    arch_ppc,    false, CPU_TYPE_POWERPC, CPU_SUBTYPE_POWERPC_ALL },
  { "armv6",  arch_armv6,  true,  CPU_TYPE_ARM,     CPU_SUBTYPE_ARM_V6 },
  { "armv7",  arch_armv7,  true,  CPU_TYPE_ARM,     CPU_SUBTYPE_ARM_V7 },
  { "armv7s", arch_armv7s, true,  CPU_TYPE_ARM,     CPU_SUBTYPE_ARM_V7S },
  { "",       arch_unknown,false, 0,                0 }
};

MachOLinkingContext::Arch
MachOLinkingContext::archFromCpuType(uint32_t cputype, uint32_t cpusubtype) {
  for (ArchInfo *info = _s_archInfos; !info->archName.empty(); ++info) {
    if ((info->cputype == cputype) && (info->cpusubtype == cpusubtype))
      return info->arch;
  }
  return arch_unknown;
}

MachOLinkingContext::Arch
MachOLinkingContext::archFromName(StringRef archName) {
  for (ArchInfo *info = _s_archInfos; !info->archName.empty(); ++info) {
    if (info->archName.equals(archName))
      return info->arch;
  }
  return arch_unknown;
}

StringRef MachOLinkingContext::nameFromArch(Arch arch) {
  for (ArchInfo *info = _s_archInfos; !info->archName.empty(); ++info) {
    if (info->arch == arch)
      return info->archName;
  }
  return "<unknown>";
}

uint32_t MachOLinkingContext::cpuTypeFromArch(Arch arch) {
  assert(arch != arch_unknown);
  for (ArchInfo *info = _s_archInfos; !info->archName.empty(); ++info) {
    if (info->arch == arch)
      return info->cputype;
  }
  llvm_unreachable("Unknown arch type");
}

uint32_t MachOLinkingContext::cpuSubtypeFromArch(Arch arch) {
  assert(arch != arch_unknown);
  for (ArchInfo *info = _s_archInfos; !info->archName.empty(); ++info) {
    if (info->arch == arch)
      return info->cpusubtype;
  }
  llvm_unreachable("Unknown arch type");
}

MachOLinkingContext::MachOLinkingContext()
    : _outputMachOType(MH_EXECUTE), _outputMachOTypeStatic(false),
      _doNothing(false), _arch(arch_unknown), _os(OS::macOSX), _osMinVersion(0),
      _pageZeroSize(0), _pageSize(4096), _compatibilityVersion(0),
      _currentVersion(0), _deadStrippableDylib(false), _printAtoms(false),
      _testingLibResolution(false), _archHandler(nullptr) {}

MachOLinkingContext::~MachOLinkingContext() {}

void MachOLinkingContext::configure(HeaderFileType type, Arch arch, OS os,
                                    uint32_t minOSVersion) {
  _outputMachOType = type;
  _arch = arch;
  _os = os;
  _osMinVersion = minOSVersion;

  switch (_outputMachOType) {
  case llvm::MachO::MH_EXECUTE:
    // If targeting newer OS, use _main
    if (minOS("10.8", "6.0")) {
      _entrySymbolName = "_main";
    } else {
      // If targeting older OS, use start (in crt1.o)
      _entrySymbolName = "start";
    }

    // __PAGEZERO defaults to 4GB on 64-bit (except for PP64 which lld does not
    // support) and 4KB on 32-bit.
    if (is64Bit(_arch)) {
      _pageZeroSize = 0x100000000;
    } else {
      _pageZeroSize = 0x1000;
    }

    break;
  case llvm::MachO::MH_DYLIB:
    _globalsAreDeadStripRoots = true;
    break;
  case llvm::MachO::MH_BUNDLE:
    break;
  case llvm::MachO::MH_OBJECT:
    _printRemainingUndefines = false;
    _allowRemainingUndefines = true;
  default:
    break;
  }
}

uint32_t MachOLinkingContext::getCPUType() const {
  return cpuTypeFromArch(_arch);
}

uint32_t MachOLinkingContext::getCPUSubType() const {
  return cpuSubtypeFromArch(_arch);
}

bool MachOLinkingContext::is64Bit(Arch arch) {
  for (ArchInfo *info = _s_archInfos; !info->archName.empty(); ++info) {
    if (info->arch == arch) {
      return (info->cputype & CPU_ARCH_ABI64);
    }
  }
  // unknown archs are not 64-bit.
  return false;
}

bool MachOLinkingContext::isHostEndian(Arch arch) {
  assert(arch != arch_unknown);
  for (ArchInfo *info = _s_archInfos; !info->archName.empty(); ++info) {
    if (info->arch == arch) {
      return (info->littleEndian == llvm::sys::IsLittleEndianHost);
    }
  }
  llvm_unreachable("Unknown arch type");
}

bool MachOLinkingContext::isBigEndian(Arch arch) {
  assert(arch != arch_unknown);
  for (ArchInfo *info = _s_archInfos; !info->archName.empty(); ++info) {
    if (info->arch == arch) {
      return ! info->littleEndian;
    }
  }
  llvm_unreachable("Unknown arch type");
}



bool MachOLinkingContext::is64Bit() const {
  return is64Bit(_arch);
}

bool MachOLinkingContext::outputTypeHasEntry() const {
  switch (_outputMachOType) {
  case MH_EXECUTE:
  case MH_DYLINKER:
  case MH_PRELOAD:
    return true;
  default:
    return false;
  }
}

bool MachOLinkingContext::needsStubsPass() const {
  switch (_outputMachOType) {
  case MH_EXECUTE:
    return !_outputMachOTypeStatic;
  case MH_DYLIB:
  case MH_BUNDLE:
    return true;
  default:
    return false;
  }
}

bool MachOLinkingContext::needsGOTPass() const {
  // Only x86_64 uses GOT pass but not in -r mode.
  if (_arch != arch_x86_64)
    return false;
  return (_outputMachOType != MH_OBJECT);
}


StringRef MachOLinkingContext::binderSymbolName() const {
  return archHandler().stubInfo().binderSymbolName;
}




bool MachOLinkingContext::minOS(StringRef mac, StringRef iOS) const {
  uint32_t parsedVersion;
  switch (_os) {
  case OS::macOSX:
    if (parsePackedVersion(mac, parsedVersion))
      return false;
    return _osMinVersion >= parsedVersion;
  case OS::iOS:
  case OS::iOS_simulator:
    if (parsePackedVersion(iOS, parsedVersion))
      return false;
    return _osMinVersion >= parsedVersion;
  case OS::unknown:
    break;
  }
  llvm_unreachable("target not configured for iOS or MacOSX");
}

bool MachOLinkingContext::addEntryPointLoadCommand() const {
  if ((_outputMachOType == MH_EXECUTE) && !_outputMachOTypeStatic) {
    return minOS("10.8", "6.0");
  }
  return false;
}

bool MachOLinkingContext::addUnixThreadLoadCommand() const {
  switch (_outputMachOType) {
  case MH_EXECUTE:
    if (_outputMachOTypeStatic)
      return true;
    else
      return !minOS("10.8", "6.0");
    break;
  case MH_DYLINKER:
  case MH_PRELOAD:
    return true;
  default:
    return false;
  }
}

bool MachOLinkingContext::pathExists(StringRef path) const {
  if (!testingLibResolution())
    return llvm::sys::fs::exists(path.str());

  // Otherwise, we're in test mode: only files explicitly provided on the
  // command-line exist.
  return _existingPaths.find(path) != _existingPaths.end();
}

void MachOLinkingContext::addModifiedSearchDir(
    StringRef libPath, const StringRefVector &syslibRoots, bool isSystemPath) {
  bool addedModifiedPath = false;

  // Two cases to consider here:
  //   + If the last -syslibroot is "/", all of them are ignored (don't ask).
  //   + -syslibroot only applies to absolute paths.
  if (!syslibRoots.empty() && syslibRoots.back() != "/" &&
      llvm::sys::path::is_absolute(libPath)) {
    for (auto syslibRoot : syslibRoots) {
      SmallString<256> path(syslibRoot);
      llvm::sys::path::append(path, libPath);
      if (pathExists(path)) {
        _searchDirs.push_back(path.str().copy(_allocator));
        addedModifiedPath = true;
      }
    }
  }

  if (addedModifiedPath)
    return;

  // Finally, if only one -syslibroot is given, system paths which aren't in it
  // get suppressed.
  if (syslibRoots.size() != 1 || !isSystemPath) {
    if (pathExists(libPath)) {
      _searchDirs.push_back(libPath);
    }
  }
}

ErrorOr<StringRef>
MachOLinkingContext::searchDirForLibrary(StringRef path,
                                         StringRef libName) const {
  SmallString<256> fullPath;
  if (libName.endswith(".o")) {
    // A request ending in .o is special: just search for the file directly.
    fullPath.assign(path);
    llvm::sys::path::append(fullPath, libName);
    if (pathExists(fullPath))
      return fullPath.str().copy(_allocator);
    return make_error_code(llvm::errc::no_such_file_or_directory);
  }

  // Search for dynamic library
  fullPath.assign(path);
  llvm::sys::path::append(fullPath, Twine("lib") + libName + ".dylib");
  if (pathExists(fullPath))
    return fullPath.str().copy(_allocator);

  // If not, try for a static library
  fullPath.assign(path);
  llvm::sys::path::append(fullPath, Twine("lib") + libName + ".a");
  if (pathExists(fullPath))
    return fullPath.str().copy(_allocator);

  return make_error_code(llvm::errc::no_such_file_or_directory);
}



ErrorOr<StringRef> MachOLinkingContext::searchLibrary(StringRef libName) const {
  SmallString<256> path;
  for (StringRef dir : searchDirs()) {
    ErrorOr<StringRef> ec = searchDirForLibrary(dir, libName);
    if (ec)
      return ec;
  }

  return make_error_code(llvm::errc::no_such_file_or_directory);
}

bool MachOLinkingContext::validateImpl(raw_ostream &diagnostics) {
  // TODO: if -arch not specified, look at arch of first .o file.

  if (_currentVersion && _outputMachOType != MH_DYLIB) {
    diagnostics << "error: -current_version can only be used with dylibs\n";
    return false;
  }

  if (_compatibilityVersion && _outputMachOType != MH_DYLIB) {
    diagnostics
        << "error: -compatibility_version can only be used with dylibs\n";
    return false;
  }

  if (_deadStrippableDylib && _outputMachOType != MH_DYLIB) {
    diagnostics
        << "error: -mark_dead_strippable_dylib can only be used with dylibs.\n";
    return false;
  }

  if (!_bundleLoader.empty() && outputMachOType() != MH_BUNDLE) {
    diagnostics
        << "error: -bundle_loader can only be used with Mach-O bundles\n";
    return false;
  }

  return true;
}

void MachOLinkingContext::addPasses(PassManager &pm) {
  pm.add(std::unique_ptr<Pass>(new LayoutPass(registry())));
  if (needsStubsPass())
    mach_o::addStubsPass(pm, *this);
  if (needsGOTPass())
    mach_o::addGOTPass(pm, *this);
}

Writer &MachOLinkingContext::writer() const {
  if (!_writer)
    _writer = createWriterMachO(*this);
  return *_writer;
}

ArchHandler &MachOLinkingContext::archHandler() const {
  if (!_archHandler)
    _archHandler = ArchHandler::create(_arch);
  return *_archHandler;
}


} // end namespace lld
