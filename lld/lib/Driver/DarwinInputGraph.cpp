//===- lib/ReaderWriter/MachO/DarwinInputGraph.cpp ------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Driver/DarwinInputGraph.h"
#include "lld/Core/ArchiveLibraryFile.h"
#include "lld/Core/DefinedAtom.h"
#include "lld/Core/File.h"
#include "lld/Core/LLVM.h"
#include "lld/Core/Reference.h"
#include "lld/Core/SharedLibraryFile.h"

namespace lld {


/// \brief Parse the input file to lld::File.
std::error_code MachOFileNode::parse(const LinkingContext &ctx,
                                     raw_ostream &diagnostics)  {
  ErrorOr<StringRef> filePath = getPath(ctx);
  if (std::error_code ec = filePath.getError())
    return ec;
  ErrorOr<std::unique_ptr<MemoryBuffer>> mbOrErr =
      MemoryBuffer::getFileOrSTDIN(*filePath);
  if (std::error_code ec = mbOrErr.getError())
    return ec;
  std::unique_ptr<MemoryBuffer> mb = std::move(mbOrErr.get());

  _context.addInputFileDependency(*filePath);
  if (ctx.logInputFiles())
    diagnostics << *filePath << "\n";

  narrowFatBuffer(mb, *filePath);

  std::vector<std::unique_ptr<File>> parsedFiles;
  if (std::error_code ec = ctx.registry().parseFile(std::move(mb), parsedFiles))
    return ec;
  for (std::unique_ptr<File> &pf : parsedFiles) {
    // If file is a dylib, inform LinkingContext about it.
    if (SharedLibraryFile *shl = dyn_cast<SharedLibraryFile>(pf.get())) {
      _context.registerDylib(reinterpret_cast<mach_o::MachODylibFile*>(shl),
                             _upwardDylib);
    }
    // If file is an archive and -all_load, then add all members.
    if (ArchiveLibraryFile *archive = dyn_cast<ArchiveLibraryFile>(pf.get())) {
      if (_isWholeArchive) {
        // Have this node own the FileArchive object.
        _archiveFile.reset(archive);
        pf.release();
        // Add all members to _files vector
        return archive->parseAllMembers(_files);
      }
    }
    _files.push_back(std::move(pf));
  }
  return std::error_code();
}


/// If buffer contains a fat file, find required arch in fat buffer and
/// switch buffer to point to just that required slice.
void MachOFileNode::narrowFatBuffer(std::unique_ptr<MemoryBuffer> &mb,
                                    StringRef filePath) {
  // Check if buffer is a "fat" file that contains needed arch.
  uint32_t offset;
  uint32_t size;
  if (!_context.sliceFromFatFile(*mb, offset, size)) {
    return;
  }
  // Create new buffer containing just the needed slice.
  auto subuf = MemoryBuffer::getFileSlice(filePath, size, offset);
  if (subuf.getError())
    return;
  // The assignment to mb will release previous buffer.
  mb = std::move(subuf.get());
}


} // end namesapce lld
