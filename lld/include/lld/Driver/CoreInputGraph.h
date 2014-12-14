//===- lld/Driver/CoreInputGraph.h - Input Graph Node for Core linker -----===//
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
/// Handles Options for CORE linking and provides InputElements
/// for the CORE linker
///
//===----------------------------------------------------------------------===//

#ifndef LLD_DRIVER_CORE_INPUT_GRAPH_H
#define LLD_DRIVER_CORE_INPUT_GRAPH_H

#include "lld/Core/InputGraph.h"
#include "lld/ReaderWriter/CoreLinkingContext.h"
#include "lld/ReaderWriter/Reader.h"
#include "llvm/Support/Errc.h"
#include <map>
#include <memory>

namespace lld {

/// \brief Represents a Core File
class CoreFileNode : public FileNode {
public:
  CoreFileNode(CoreLinkingContext &, StringRef path) : FileNode(path) {}

  /// \brief Parse the input file to lld::File.
  std::error_code parse(const LinkingContext &ctx,
                        raw_ostream &diagnostics) override {
    ErrorOr<StringRef> filePath = getPath(ctx);
    if (filePath.getError() == llvm::errc::no_such_file_or_directory)
      return make_error_code(llvm::errc::no_such_file_or_directory);

    // Create a memory buffer
    ErrorOr<std::unique_ptr<MemoryBuffer>> mb =
        MemoryBuffer::getFileOrSTDIN(*filePath);
    if (std::error_code ec = mb.getError())
      return ec;

    return ctx.registry().parseFile(std::move(mb.get()), _files);
  }
};

} // namespace lld

#endif
