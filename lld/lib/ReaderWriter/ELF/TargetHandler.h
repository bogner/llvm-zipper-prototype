//===- lib/ReaderWriter/ELF/TargetHandler.h -------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief These interfaces provide target specific hooks to change the linker's
/// behaivor.
///
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_TARGET_HANDLER_H
#define LLD_READER_WRITER_ELF_TARGET_HANDLER_H

#include "Layout.h"

#include "lld/Core/LLVM.h"
#include "lld/Core/LinkingContext.h"
#include "lld/Core/STDExtras.h"
#include "lld/ReaderWriter/ELFLinkingContext.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/Support/FileOutputBuffer.h"

#include <memory>
#include <vector>

namespace lld {
namespace elf {
template <class ELFT> class DynamicTable;
template <class ELFT> class DynamicSymbolTable;
template <class ELFT> class ELFDefinedAtom;
template <class ELFT> class ELFReference;
class ELFWriter;
template <class ELFT> class ELFHeader;
template <class ELFT> class Section;
template <class ELFT> class TargetLayout;

template <class ELFT> class TargetRelocationHandler {
public:
  virtual error_code
  applyRelocation(ELFWriter &, llvm::FileOutputBuffer &,
                  const lld::AtomLayout &, const Reference &) const = 0;

  virtual ~TargetRelocationHandler() {}
};

/// \brief An interface to override functions that are provided by the
/// the default ELF Layout
template <class ELFT> class TargetHandler : public TargetHandlerBase {

public:
  TargetHandler(ELFLinkingContext &targetInfo) : _context(targetInfo) {}

  /// If the target overrides ELF header information, this API would
  /// return true, so that the target can set all fields specific to
  /// that target
  virtual bool doesOverrideELFHeader() = 0;

  /// Set the ELF Header information
  virtual void setELFHeader(ELFHeader<ELFT> *elfHeader) = 0;

  /// TargetLayout
  virtual TargetLayout<ELFT> &targetLayout() = 0;

  virtual const TargetRelocationHandler<ELFT> &getRelocationHandler() const = 0;

  /// Create a set of Default target sections that a target might needj
  virtual void createDefaultSections() = 0;

  /// \brief Add a section to the current Layout
  virtual void addSection(Section<ELFT> *section) = 0;

  /// \brief add new symbol file
  virtual bool createImplicitFiles(std::vector<std::unique_ptr<File> > &) = 0;

  /// \brief Finalize the symbol values
  virtual void finalizeSymbolValues() = 0;

  /// \brief allocate Commons, some architectures may move small common
  /// symbols over to small data, this would also be used
  virtual void allocateCommons() = 0;

  /// \brief create dynamic table
  virtual LLD_UNIQUE_BUMP_PTR(DynamicTable<ELFT>) createDynamicTable() = 0;

  /// \brief create dynamic symbol table
  virtual LLD_UNIQUE_BUMP_PTR(DynamicSymbolTable<ELFT>)
  createDynamicSymbolTable() = 0;

  virtual std::unique_ptr<Reader> getObjReader(bool) = 0;

  virtual std::unique_ptr<Reader> getDSOReader(bool) = 0;

  virtual std::unique_ptr<Writer> getWriter() = 0;

protected:
  ELFLinkingContext &_context;
};
} // end namespace elf
} // end namespace lld

#endif
