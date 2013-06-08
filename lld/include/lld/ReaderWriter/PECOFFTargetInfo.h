//===- lld/ReaderWriter/PECOFFTargetInfo.h ---------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_PECOFF_TARGET_INFO_H
#define LLD_READER_WRITER_PECOFF_TARGET_INFO_H

#include "lld/Core/TargetInfo.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/Support/Allocator.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/ErrorHandling.h"

namespace lld {

class PECOFFTargetInfo : public TargetInfo {
public:
  PECOFFTargetInfo()
      : _stackReserve(1024 * 1024),
        _stackCommit(4096),
        _subsystem(llvm::COFF::IMAGE_SUBSYSTEM_UNKNOWN),
        _minOSVersion(6, 0) {}

  struct OSVersion {
    OSVersion(int v1, int v2) : majorVersion(v1), minorVersion(v2) {}
    int majorVersion;
    int minorVersion;
  };

  virtual error_code parseFile(
      std::unique_ptr<MemoryBuffer> &mb,
      std::vector<std::unique_ptr<File>> &result) const;

  virtual Writer &writer() const;
  virtual bool validate(raw_ostream &diagnostics);

  virtual void addPasses(PassManager &pm) const {}

  void setStackReserve(uint64_t size) { _stackReserve = size; }
  uint64_t getStackReserve() const { return _stackReserve; }

  void setStackCommit(uint64_t size) { _stackCommit = size; }
  uint64_t getStackCommit() const { return _stackCommit; }

  void setSubsystem(llvm::COFF::WindowsSubsystem ss) { _subsystem = ss; }
  llvm::COFF::WindowsSubsystem getSubsystem() const { return _subsystem; }

  void setMinOSVersion(const OSVersion &version) { _minOSVersion = version; }
  OSVersion getMinOSVersion() const { return _minOSVersion; }

  virtual ErrorOr<Reference::Kind> relocKindFromString(StringRef str) const;
  virtual ErrorOr<std::string> stringFromRelocKind(Reference::Kind kind) const;

  StringRef allocateString(const StringRef &ref) {
    char *x = _extraStrings.Allocate<char>(ref.size() + 1);
    memcpy(x, ref.data(), ref.size());
    x[ref.size()] = '\0';
    return x;
  }

private:
  uint64_t _stackReserve;
  uint64_t _stackCommit;
  llvm::COFF::WindowsSubsystem _subsystem;
  OSVersion _minOSVersion;

  mutable std::unique_ptr<Reader> _reader;
  mutable std::unique_ptr<Writer> _writer;
  llvm::BumpPtrAllocator _extraStrings;
};

} // end namespace lld

#endif
