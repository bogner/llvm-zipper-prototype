//===- lld/ReaderWriter/PECOFFLinkingContext.h ----------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_PECOFF_LINKER_CONTEXT_H
#define LLD_READER_WRITER_PECOFF_LINKER_CONTEXT_H

#include <vector>

#include "lld/Core/LinkingContext.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/Support/Allocator.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/ErrorHandling.h"

namespace lld {

class PECOFFLinkingContext : public LinkingContext {
public:
  PECOFFLinkingContext()
      : _baseAddress(0x400000), _stackReserve(1024 * 1024), _stackCommit(4096),
        _heapReserve(1024 * 1024), _heapCommit(4096),
        _subsystem(llvm::COFF::IMAGE_SUBSYSTEM_UNKNOWN), _minOSVersion(6, 0),
        _nxCompat(true), _largeAddressAware(false),
        _baseRelocationEnabled(true), _terminalServerAware(true) {}

  struct OSVersion {
    OSVersion(int v1, int v2) : majorVersion(v1), minorVersion(v2) {}
    int majorVersion;
    int minorVersion;
  };

  /// \brief Casting support
  static inline bool classof(const LinkingContext *info) { return true; }

  virtual error_code
  parseFile(std::unique_ptr<MemoryBuffer> &mb,
            std::vector<std::unique_ptr<File> > &result) const;

  virtual Writer &writer() const;
  virtual bool validateImpl(raw_ostream &diagnostics);

  virtual void addPasses(PassManager &pm) const;

  virtual void addImplicitFiles(InputFiles &) const;

  void appendInputSearchPath(StringRef dirPath) {
    _inputSearchPaths.push_back(dirPath);
  }

  const std::vector<StringRef> getInputSearchPaths() {
    return _inputSearchPaths;
  }

  StringRef searchLibraryFile(StringRef path) const;

  void setBaseAddress(uint64_t addr) { _baseAddress = addr; }
  uint64_t getBaseAddress() const { return _baseAddress; }

  void setStackReserve(uint64_t size) { _stackReserve = size; }
  void setStackCommit(uint64_t size) { _stackCommit = size; }
  uint64_t getStackReserve() const { return _stackReserve; }
  uint64_t getStackCommit() const { return _stackCommit; }

  void setHeapReserve(uint64_t size) { _heapReserve = size; }
  void setHeapCommit(uint64_t size) { _heapCommit = size; }
  uint64_t getHeapReserve() const { return _heapReserve; }
  uint64_t getHeapCommit() const { return _heapCommit; }

  void setSubsystem(llvm::COFF::WindowsSubsystem ss) { _subsystem = ss; }
  llvm::COFF::WindowsSubsystem getSubsystem() const { return _subsystem; }

  void setMinOSVersion(const OSVersion &version) { _minOSVersion = version; }
  OSVersion getMinOSVersion() const { return _minOSVersion; }

  void setNxCompat(bool nxCompat) { _nxCompat = nxCompat; }
  bool isNxCompat() const { return _nxCompat; }

  void setLargeAddressAware(bool val) { _largeAddressAware = val; }
  bool getLargeAddressAware() const { return _largeAddressAware; }

  void setBaseRelocationEnabled(bool val) { _baseRelocationEnabled = val; }
  bool getBaseRelocationEnabled() const { return _baseRelocationEnabled; }

  void setTerminalServerAware(bool val) { _terminalServerAware = val; }
  bool isTerminalServerAware() const { return _terminalServerAware; }

  virtual ErrorOr<Reference::Kind> relocKindFromString(StringRef str) const;
  virtual ErrorOr<std::string> stringFromRelocKind(Reference::Kind kind) const;

  StringRef allocateString(StringRef ref) {
    char *x = _alloc.Allocate<char>(ref.size() + 1);
    memcpy(x, ref.data(), ref.size());
    x[ref.size()] = '\0';
    return x;
  }

  virtual bool hasInputGraph() {
    if (_inputGraph)
      return true;
    return false;
  }

private:
  // The start address for the program. The default value for the executable is
  // 0x400000, but can be altered using -base command line option.
  uint64_t _baseAddress;

  uint64_t _stackReserve;
  uint64_t _stackCommit;
  uint64_t _heapReserve;
  uint64_t _heapCommit;
  llvm::COFF::WindowsSubsystem _subsystem;
  OSVersion _minOSVersion;
  bool _nxCompat;
  bool _largeAddressAware;
  bool _baseRelocationEnabled;
  bool _terminalServerAware;

  std::vector<StringRef> _inputSearchPaths;
  mutable std::unique_ptr<Reader> _reader;
  mutable std::unique_ptr<Writer> _writer;
  mutable llvm::BumpPtrAllocator _alloc;
};

} // end namespace lld

#endif
