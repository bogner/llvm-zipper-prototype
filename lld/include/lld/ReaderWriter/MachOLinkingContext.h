//===- lld/ReaderWriter/MachOLinkingContext.h -----------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_MACHO_LINKING_CONTEXT_H
#define LLD_READER_WRITER_MACHO_LINKING_CONTEXT_H

#include "lld/Core/LinkingContext.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MachO.h"

using llvm::MachO::HeaderFileType;

namespace lld {

namespace mach_o {
class KindHandler; // defined in lib. this header is in include.
}

class MachOLinkingContext : public LinkingContext {
public:
  MachOLinkingContext();
  ~MachOLinkingContext();

  enum Arch {
    arch_unknown,
    arch_ppc,
    arch_x86,
    arch_x86_64,
    arch_armv6,
    arch_armv7,
    arch_armv7s,
  };

  enum class OS {
    unknown,
    macOSX,
    iOS,
    iOS_simulator
  };

  /// Initializes the context to sane default values given the specified output
  /// file type, arch, os, and minimum os version.  This should be called before
  /// other setXXX() methods.
  void configure(HeaderFileType type, Arch arch, OS os, uint32_t minOSVersion);

  void addPasses(PassManager &pm) override;
  bool validateImpl(raw_ostream &diagnostics) override;

  uint32_t getCPUType() const;
  uint32_t getCPUSubType() const;

  bool addEntryPointLoadCommand() const;
  bool addUnixThreadLoadCommand() const;
  bool outputTypeHasEntry() const;
  bool is64Bit() const;

  virtual uint64_t pageZeroSize() const { return _pageZeroSize; }
  virtual uint64_t pageSize() const { return _pageSize; }

  mach_o::KindHandler &kindHandler() const;

  HeaderFileType outputMachOType() const { return _outputMachOType; }

  Arch arch() const { return _arch; }
  StringRef archName() const { return nameFromArch(_arch); }
  OS os() const { return _os; }

  bool minOS(StringRef mac, StringRef iOS) const;
  void setDoNothing(bool value) { _doNothing = value; }
  bool doNothing() const { return _doNothing; }
  bool printAtoms() const { return _printAtoms; }

  /// \brief The dylib's binary compatibility version, in the raw uint32 format.
  ///
  /// When building a dynamic library, this is the compatibility version that
  /// gets embedded into the result. Other Mach-O binaries that link against
  /// this library will store the compatibility version in its load command. At
  /// runtime, the loader will verify that the binary is compatible with the
  /// installed dynamic library.
  uint32_t compatibilityVersion() const { return _compatibilityVersion; }

  /// \brief The dylib's current version, in the the raw uint32 format.
  ///
  /// When building a dynamic library, this is the current version that gets
  /// embedded into the result. Other Mach-O binaries that link against
  /// this library will store the compatibility version in its load command.
  uint32_t currentVersion() const { return _currentVersion; }

  /// \brief The dylib's install name.
  ///
  /// Binaries that link against the dylib will embed this path into the dylib
  /// load command. When loading the binaries at runtime, this is the location
  /// on disk that the loader will look for the dylib.
  StringRef installName() const { return _installName; }

  /// \brief Whether or not the dylib has side effects during initialization.
  ///
  /// Dylibs marked as being dead strippable provide the guarantee that loading
  /// the dylib has no side effects, allowing the linker to strip out the dylib
  /// when linking a binary that does not use any of its symbols.
  bool deadStrippableDylib() const { return _deadStrippableDylib; }

  /// \brief The path to the executable that will load the bundle at runtime.
  ///
  /// When building a Mach-O bundle, this executable will be examined if there
  /// are undefined symbols after the main link phase. It is expected that this
  /// binary will be loading the bundle at runtime and will provide the symbols
  /// at that point.
  StringRef bundleLoader() const { return _bundleLoader; }

  void setCompatibilityVersion(uint32_t vers) { _compatibilityVersion = vers; }
  void setCurrentVersion(uint32_t vers) { _currentVersion = vers; }
  void setInstallName(StringRef name) { _installName = name; }
  void setDeadStrippableDylib(bool deadStrippable) {
    _deadStrippableDylib = deadStrippable;
  }
  void setBundleLoader(StringRef loader) { _bundleLoader = loader; }
  void setPrintAtoms(bool value=true) { _printAtoms = value; }
  StringRef dyldPath() const { return "/usr/lib/dyld"; }

  static Arch archFromCpuType(uint32_t cputype, uint32_t cpusubtype);
  static Arch archFromName(StringRef archName);
  static StringRef nameFromArch(Arch arch);
  static uint32_t cpuTypeFromArch(Arch arch);
  static uint32_t cpuSubtypeFromArch(Arch arch);
  static bool is64Bit(Arch arch);
  static bool isHostEndian(Arch arch);
  static bool isBigEndian(Arch arch);

  /// Construct 32-bit value from string "X.Y.Z" where
  /// bits are xxxx.yy.zz.  Largest number is 65535.255.255
  static bool parsePackedVersion(StringRef str, uint32_t &result);

private:
  Writer &writer() const override;

  struct ArchInfo {
    StringRef                 archName;
    MachOLinkingContext::Arch arch;
    bool                      littleEndian;
    uint32_t                  cputype;
    uint32_t                  cpusubtype;
  };

  static ArchInfo _s_archInfos[];

  HeaderFileType _outputMachOType;   // e.g MH_EXECUTE
  bool _outputMachOTypeStatic; // Disambiguate static vs dynamic prog
  bool _doNothing;            // for -help and -v which just print info
  Arch _arch;
  OS _os;
  uint32_t _osMinVersion;
  uint64_t _pageZeroSize;
  uint64_t _pageSize;
  uint32_t _compatibilityVersion;
  uint32_t _currentVersion;
  StringRef _installName;
  bool _deadStrippableDylib;
  bool _printAtoms;
  StringRef _bundleLoader;
  mutable std::unique_ptr<mach_o::KindHandler> _kindHandler;
  mutable std::unique_ptr<Writer> _writer;
};

} // end namespace lld

#endif
