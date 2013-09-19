//===- lld/Driver/DarwinInputGraph.h - Input Graph Node for Mach-O linker -===//
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
/// Handles Options for MachO linking and provides InputElements
/// for MachO linker
///
//===----------------------------------------------------------------------===//

#ifndef LLD_DRIVER_DARWIN_INPUT_GRAPH_H
#define LLD_DRIVER_DARWIN_INPUT_GRAPH_H

#include "lld/Driver/InputGraph.h"
#include "lld/ReaderWriter/MachOLinkingContext.h"

#include <map>

namespace lld {

/// \brief Represents a MachO File
class MachOFileNode : public FileNode {
public:
  MachOFileNode(MachOLinkingContext &ctx, StringRef path, bool isWholeArchive)
      : FileNode(path), _ctx(ctx), _isWholeArchive(isWholeArchive) {}

  static inline bool classof(const InputElement *a) {
    return a->kind() == InputElement::Kind::File;
  }

  virtual llvm::ErrorOr<std::unique_ptr<lld::LinkerInput> >
  createLinkerInput(const lld::LinkingContext &);

  /// \brief validates the Input Element
  virtual bool validate() {
    (void)_ctx;
    return true;
  }

  /// \brief Dump the Input Element
  virtual bool dump(raw_ostream &) { return true; }

private:
  const MachOLinkingContext &_ctx;
  bool _isWholeArchive;
};

} // namespace lld

#endif
