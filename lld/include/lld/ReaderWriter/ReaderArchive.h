//===- lld/ReaderWriter/ReaderArchive.h - Archive Library Reader ----------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_ARCHIVE_H
#define LLD_READER_ARCHIVE_H

#include "lld/Core/LLVM.h"
#include "lld/ReaderWriter/Reader.h"

#include "llvm/Object/Archive.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/system_error.h"

#include <functional>
#include <memory>
#include <vector>

namespace lld {
class File;
class LinkingContext;
class LinkerInput;

/// \brief ReaderArchive is a class for reading archive libraries
class ReaderArchive : public Reader {
public:
  ReaderArchive(const LinkingContext &context, const Reader &memberReader)
      : Reader(context) {}

  /// \brief Returns a vector of Files that are contained in the archive file
  ///        pointed to by the Memorybuffer
  error_code parseFile(std::unique_ptr<llvm::MemoryBuffer> &mb,
                       std::vector<std::unique_ptr<File>> &result) const;

private:
  mutable std::unique_ptr<llvm::object::Archive> _archive;
};
} // end namespace lld

#endif
