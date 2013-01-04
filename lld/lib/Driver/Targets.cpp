//===- lib/Driver/Targets.cpp - Linker Targets ----------------------------===//
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
/// Concrete instances of the Target interface.
///
//===----------------------------------------------------------------------===//

#include "lld/Driver/Target.h"

#include "lld/ReaderWriter/ReaderArchive.h"
#include "lld/ReaderWriter/ReaderELF.h"
#include "lld/ReaderWriter/ReaderYAML.h"
#include "lld/ReaderWriter/WriterELF.h"

#include "llvm/ADT/Triple.h"

using namespace lld;

class X86LinuxTarget final : public Target {
public:
  X86LinuxTarget(const LinkerOptions &lo) : Target(lo), _woe(lo._entrySymbol) {
    _readerELF.reset(createReaderELF(_roe, _roa));
    _readerYAML.reset(createReaderYAML(_roy));
    _writer.reset(createWriterELF(_woe));
  }

  virtual ErrorOr<lld::Reader&> getReader(const LinkerInput &input) {
    auto kind = input.getKind();
    if (!kind)
      return error_code(kind);

    if (*kind == InputKind::YAML)
      return *_readerYAML;

    if (*kind == InputKind::Object)
      return *_readerELF;

    return llvm::make_error_code(llvm::errc::invalid_argument);
  }

  virtual ErrorOr<lld::Writer&> getWriter() {
    return *_writer;
  }

private:
  lld::ReaderOptionsELF _roe;
  lld::ReaderOptionsArchive _roa;
  struct : lld::ReaderOptionsYAML {
    virtual Reference::Kind kindFromString(StringRef kindName) const {
      int k;
      if (kindName.getAsInteger(0, k))
        k = 0;
      return k;
    }
  } _roy;

  struct WOpts : lld::WriterOptionsELF {
    WOpts(StringRef entry) {
      _endianness = llvm::support::little;
      _is64Bit = false;
      _type = llvm::ELF::ET_EXEC;
      _machine = llvm::ELF::EM_386;
      _entryPoint = entry;
    }
  } _woe;

  std::unique_ptr<lld::Reader> _readerELF, _readerYAML;
  std::unique_ptr<lld::Writer> _writer;
};

class X86_64LinuxTarget final : public Target {
public:
  X86_64LinuxTarget(const LinkerOptions &lo)
    : Target(lo), _woe(lo._entrySymbol) {
    _readerELF.reset(createReaderELF(_roe, _roa));
    _readerYAML.reset(createReaderYAML(_roy));
    _writer.reset(createWriterELF(_woe));
  }

  virtual ErrorOr<lld::Reader&> getReader(const LinkerInput &input) {
    auto kind = input.getKind();
    if (!kind)
      return error_code(kind);

    if (*kind == InputKind::YAML)
      return *_readerYAML;

    if (*kind == InputKind::Object)
      return *_readerELF;

    return llvm::make_error_code(llvm::errc::invalid_argument);
  }

  virtual ErrorOr<lld::Writer&> getWriter() {
    return *_writer;
  }

private:
  lld::ReaderOptionsELF _roe;
  lld::ReaderOptionsArchive _roa;
  struct : lld::ReaderOptionsYAML {
    virtual Reference::Kind kindFromString(StringRef kindName) const {
      int k;
      if (kindName.getAsInteger(0, k))
        k = 0;
      return k;
    }
  } _roy;

  struct WOpts : lld::WriterOptionsELF {
    WOpts(StringRef entry) {
      _endianness = llvm::support::little;
      _is64Bit = false;
      _type = llvm::ELF::ET_EXEC;
      _machine = llvm::ELF::EM_X86_64;
      _entryPoint = entry;
    }
  } _woe;

  std::unique_ptr<lld::Reader> _readerELF, _readerYAML;
  std::unique_ptr<lld::Writer> _writer;
};

std::unique_ptr<Target> Target::create(const LinkerOptions &lo) {
  llvm::Triple t(lo._target);
  if (t.getOS() == llvm::Triple::Linux && t.getArch() == llvm::Triple::x86)
    return std::unique_ptr<Target>(new X86LinuxTarget(lo));
  else if (t.getOS() == llvm::Triple::Linux &&
           t.getArch() == llvm::Triple::x86_64)
    return std::unique_ptr<Target>(new X86_64LinuxTarget(lo));
  return std::unique_ptr<Target>();
}
