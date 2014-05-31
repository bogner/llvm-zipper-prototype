//===- lib/Driver/WinLinkInputGraph.cpp -----------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Driver/WinLinkInputGraph.h"

namespace lld {

bool isCOFFLibraryFileExtension(StringRef path) {
  return path.endswith_lower(".lib") || path.endswith_lower(".imp");
}

/// \brief Parse the input file to lld::File.
error_code PECOFFFileNode::parse(const LinkingContext &ctx,
                                 raw_ostream &diagnostics) {
  if (_parsed)
    return error_code();
  _parsed = true;
  ErrorOr<StringRef> filePath = getPath(ctx);
  if (error_code ec = filePath.getError()) {
    diagnostics << "File not found: " << _path << "\n";
    return ec;
  }

  if (error_code ec = getBuffer(*filePath)) {
    diagnostics << "Cannot open file: " << *filePath << "\n";
    return ec;
  }

  if (ctx.logInputFiles())
    diagnostics << *filePath << "\n";

  return ctx.registry().parseFile(_buffer, _files);
}

ErrorOr<File &> PECOFFFileNode::getNextFile() {
  if (_nextFileIndex == _files.size())
    return make_error_code(InputGraphError::no_more_files);
  return *_files[_nextFileIndex++];
}

ErrorOr<StringRef> PECOFFFileNode::getPath(const LinkingContext &) const {
  if (isCOFFLibraryFileExtension(_path))
    return _ctx.searchLibraryFile(_path);
  if (llvm::sys::path::extension(_path).empty())
    return _ctx.allocate(_path.str() + ".obj");
  return _path;
}

ErrorOr<StringRef> PECOFFLibraryNode::getPath(const LinkingContext &) const {
  if (isCOFFLibraryFileExtension(_path))
    return _ctx.searchLibraryFile(_path);
  return _ctx.searchLibraryFile(_ctx.allocate(_path.str() + ".lib"));
}

} // end anonymous namespace
