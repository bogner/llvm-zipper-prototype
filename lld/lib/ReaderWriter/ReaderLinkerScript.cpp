//===- lib/ReaderWriter/ReaderLinkerScript.cpp ----------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/ReaderWriter/ReaderLinkerScript.h"

#include "lld/Core/Error.h"
#include "lld/Core/File.h"
#include "lld/Core/LinkerOptions.h"
#include "lld/ReaderWriter/LinkerScript.h"

using namespace lld;
using namespace script;

namespace {
class LinkerScriptFile : public File {
public:
  static ErrorOr<std::unique_ptr<LinkerScriptFile> >
  create(const TargetInfo &ti, std::unique_ptr<llvm::MemoryBuffer> mb) {
    std::unique_ptr<LinkerScriptFile> file(
        new LinkerScriptFile(ti, std::move(mb)));
    file->_script = file->_parser.parse();
    if (!file->_script)
      return linker_script_reader_error::parse_error;
    return std::move(file);
  }

  virtual Kind kind() const { return kindLinkerScript; }

  static inline bool classof(const File *f) {
    return f->kind() == kindLinkerScript;
  }

  virtual void setOrdinalAndIncrement(uint64_t &ordinal) const {
    _ordinal = ordinal++;
  }

  virtual const TargetInfo &getTargetInfo() const { return _targetInfo; }

  virtual const atom_collection<DefinedAtom> &defined() const {
    return _definedAtoms;
  }

  virtual const atom_collection<UndefinedAtom> &undefined() const {
    return _undefinedAtoms;
  }

  virtual const atom_collection<SharedLibraryAtom> &sharedLibrary() const {
    return _sharedLibraryAtoms;
  }

  virtual const atom_collection<AbsoluteAtom> &absolute() const {
    return _absoluteAtoms;
  }

  const LinkerScript *getScript() {
    return _script;
  }

private:
  LinkerScriptFile(const TargetInfo &ti, std::unique_ptr<llvm::MemoryBuffer> mb)
      : File(mb->getBufferIdentifier()),
        _targetInfo(ti),
        _lexer(std::move(mb)),
        _parser(_lexer),
        _script(nullptr) {}

  const TargetInfo &_targetInfo;
  atom_collection_vector<DefinedAtom> _definedAtoms;
  atom_collection_vector<UndefinedAtom> _undefinedAtoms;
  atom_collection_vector<SharedLibraryAtom> _sharedLibraryAtoms;
  atom_collection_vector<AbsoluteAtom> _absoluteAtoms;
  Lexer _lexer;
  Parser _parser;
  const LinkerScript *_script;
};
} // end anon namespace

namespace lld {
error_code
ReaderLinkerScript::parseFile(std::unique_ptr<llvm::MemoryBuffer> mb,
                              std::vector<std::unique_ptr<File> > &result) {
  auto lsf = LinkerScriptFile::create(_targetInfo, std::move(mb));
  if (!lsf)
    return lsf;
  const LinkerScript *ls = (*lsf)->getScript();
  result.push_back(std::move(*lsf));
  for (const auto &c : ls->_commands) {
    if (auto group = dyn_cast<Group>(c))
      for (const auto &path : group->getPaths()) {
        auto reader = _getReader(LinkerInput(path._path));
        if (!reader)
          return reader;
        if (error_code ec = reader->readFile(path._path, result))
          return ec;
      }
  }
  return error_code::success();
}
} // end namespace lld
