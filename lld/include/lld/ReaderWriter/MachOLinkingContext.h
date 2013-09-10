//===- lld/ReaderWriter/MachOLinkingContext.h -----------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_MACHO_LINKER_CONTEXT_H
#define LLD_READER_WRITER_MACHO_LINKER_CONTEXT_H

#include "lld/Core/LinkingContext.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/Support/ErrorHandling.h"

namespace lld {

namespace mach_o {
class KindHandler; // defined in lib. this header is in include.
}

class MachOLinkingContext : public LinkingContext {
public:
  MachOLinkingContext();
  ~MachOLinkingContext();

  virtual void addPasses(PassManager &pm) const;
  virtual ErrorOr<Reference::Kind> relocKindFromString(StringRef str) const;
  virtual ErrorOr<std::string> stringFromRelocKind(Reference::Kind kind) const;
  virtual bool validateImpl(raw_ostream &diagnostics);

  virtual error_code
  parseFile(LinkerInput &input,
            std::vector<std::unique_ptr<File> > &result) const;

  uint32_t getCPUType() const;
  uint32_t getCPUSubType() const;

  bool addEntryPointLoadCommand() const;
  bool addUnixThreadLoadCommand() const;
  bool outputTypeHasEntry() const;

  virtual uint64_t pageZeroSize() const { return _pageZeroSize; }

  mach_o::KindHandler &kindHandler() const;

  uint32_t outputFileType() const { return _outputFileType; }

  enum Arch {
    arch_unknown,
    arch_x86,
    arch_x86_64,
    arch_armv6,
    arch_armv7,
    arch_armv7s,
  };

  enum class OS {
    macOSX, iOS, iOS_simulator
  };

  Arch arch() const { return _arch; }
  OS os() const { return _os; }

  void setOutputFileType(uint32_t type) { _outputFileType = type; }
  void setArch(Arch arch) { _arch = arch; }
  bool setOS(OS os, StringRef minOSVersion);
  bool minOS(StringRef mac, StringRef iOS) const;
  void setDoNothing(bool value) { _doNothing = value; }
  bool doNothing() const { return _doNothing; }

  static Arch archFromCpuType(uint32_t cputype, uint32_t cpusubtype);
  static Arch archFromName(StringRef archName);
  static uint32_t cpuTypeFromArch(Arch arch);
  static uint32_t cpuSubtypeFromArch(Arch arch);

  /// Construct 32-bit value from string "X.Y.Z" where
  /// bits are xxxx.yy.zz.  Largest number is 65535.255.255
  static bool parsePackedVersion(StringRef str, uint32_t &result);

private:
  virtual Writer &writer() const;

  uint32_t _outputFileType;   // e.g MH_EXECUTE
  bool _outputFileTypeStatic; // Disambiguate static vs dynamic prog
  bool _doNothing;            // for -help and -v which just print info
  Arch _arch;
  OS _os;
  uint32_t _osMinVersion;
  uint64_t _pageZeroSize;
  mutable std::unique_ptr<mach_o::KindHandler> _kindHandler;
  mutable std::unique_ptr<Reader> _machoReader;
  mutable std::unique_ptr<Writer> _writer;
};

} // end namespace lld

#endif
