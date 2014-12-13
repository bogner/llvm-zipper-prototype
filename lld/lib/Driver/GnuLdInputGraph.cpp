//===- lib/Driver/GnuLdInputGraph.cpp -------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Driver/GnuLdInputGraph.h"
#include "lld/ReaderWriter/LinkerScript.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace lld;

llvm::ErrorOr<StringRef> ELFFileNode::getPath(const LinkingContext &) const {
  if (_attributes._isDashlPrefix)
    return _elfLinkingContext.searchLibrary(_path);
  return _elfLinkingContext.searchFile(_path, _attributes._isSysRooted);
}

std::string ELFFileNode::errStr(std::error_code errc) {
  if (errc == llvm::errc::no_such_file_or_directory) {
    if (_attributes._isDashlPrefix)
      return (Twine("Unable to find library -l") + _path).str();
    return (Twine("Unable to find file ") + _path).str();
  }
  return FileNode::errStr(errc);
}

/// \brief Parse the input file to lld::File.
std::error_code ELFFileNode::parse(const LinkingContext &ctx,
                                   raw_ostream &diagnostics) {
  ErrorOr<StringRef> filePath = getPath(ctx);
  if (std::error_code ec = filePath.getError())
    return ec;
  ErrorOr<std::unique_ptr<MemoryBuffer>> mb =
      MemoryBuffer::getFileOrSTDIN(*filePath);
  if (std::error_code ec = mb.getError())
    return ec;
  if (ctx.logInputFiles())
    diagnostics << *filePath << "\n";

  if (_attributes._isWholeArchive) {
    std::vector<std::unique_ptr<File>> parsedFiles;
    if (std::error_code ec = ctx.registry().parseFile(
            std::move(mb.get()), parsedFiles))
      return ec;
    assert(parsedFiles.size() == 1);
    std::unique_ptr<File> f(parsedFiles[0].release());
    if (const auto *archive = dyn_cast<ArchiveLibraryFile>(f.get())) {
      // Have this node own the FileArchive object.
      _archiveFile.reset(archive);
      f.release();
      // Add all members to _files vector
      return archive->parseAllMembers(_files);
    }
    // if --whole-archive is around non-archive, just use it as normal.
    _files.push_back(std::move(f));
    return std::error_code();
  }
  return ctx.registry().parseFile(std::move(mb.get()), _files);
}

/// \brief Parse the GnuLD Script
std::error_code GNULdScript::parse(const LinkingContext &ctx,
                                   raw_ostream &diagnostics) {
  ErrorOr<StringRef> filePath = getPath(ctx);
  if (std::error_code ec = filePath.getError())
    return ec;
  ErrorOr<std::unique_ptr<MemoryBuffer>> mb =
      MemoryBuffer::getFileOrSTDIN(*filePath);
  if (std::error_code ec = mb.getError())
    return ec;

  if (ctx.logInputFiles())
    diagnostics << *filePath << "\n";

  _lexer.reset(new script::Lexer(std::move(mb.get())));
  _parser.reset(new script::Parser(*_lexer.get()));

  _linkerScript = _parser->parse();

  if (!_linkerScript)
    return LinkerScriptReaderError::parse_error;

  return std::error_code();
}

static bool isPathUnderSysroot(StringRef sysroot, StringRef path) {
  if (sysroot.empty())
    return false;

  while (!path.empty() && !llvm::sys::fs::equivalent(sysroot, path))
    path = llvm::sys::path::parent_path(path);

  return !path.empty();
}

/// \brief Handle GnuLD script for ELF.
std::error_code ELFGNULdScript::parse(const LinkingContext &ctx,
                                      raw_ostream &diagnostics) {
  ELFFileNode::Attributes attributes;
  if (std::error_code ec = GNULdScript::parse(ctx, diagnostics))
    return ec;
  StringRef sysRoot = _elfLinkingContext.getSysroot();
  if (!sysRoot.empty() && isPathUnderSysroot(sysRoot, *getPath(ctx)))
    attributes.setSysRooted(true);
  for (const script::Command *c : _linkerScript->_commands) {
    auto *group = dyn_cast<script::Group>(c);
    if (!group)
      continue;
    size_t numfiles = 0;
    for (const script::Path &path : group->getPaths()) {
      // TODO : Propagate Set WholeArchive/dashlPrefix
      attributes.setAsNeeded(path._asNeeded);
      attributes.setDashlPrefix(path._isDashlPrefix);
      auto inputNode = new ELFFileNode(
          _elfLinkingContext, _elfLinkingContext.allocateString(path._path),
          attributes);
      std::unique_ptr<InputElement> inputFile(inputNode);
      _expandElements.push_back(std::move(inputFile));
      ++numfiles;
    }
    _expandElements.push_back(llvm::make_unique<GroupEnd>(numfiles));
  }
  return std::error_code();
}
