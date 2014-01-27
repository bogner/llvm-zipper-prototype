//===- lib/ReaderWriter/ELF/DefaultTargetHandler.h ------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_DEFAULT_TARGET_HANDLER_H
#define LLD_READER_WRITER_ELF_DEFAULT_TARGET_HANDLER_H

#include "DefaultLayout.h"
#include "TargetHandler.h"
#include "ELFReader.h"

#include "lld/ReaderWriter/ELFLinkingContext.h"

#include "llvm/ADT/Triple.h"
#include "llvm/Support/ELF.h"

namespace lld {
namespace elf {
template <class ELFT>
class DefaultTargetHandler : public TargetHandler<ELFT> {
public:
  DefaultTargetHandler(ELFLinkingContext &context)
      : TargetHandler<ELFT>(context) {}

  bool doesOverrideELFHeader() { return false; }

  void setELFHeader(ELFHeader<ELFT> *elfHeader) {
    llvm_unreachable("Target should provide implementation for function ");
  }

  const TargetRelocationHandler<ELFT> &getRelocationHandler() const {
    llvm_unreachable("Target should provide implementation for function ");
  }

  /// Create a set of Default target sections that a target might needj
  void createDefaultSections() {}

  /// \brief Add a section to the current Layout
  void addSection(Section<ELFT> *section) {}

  /// \brief add new symbol file
  bool createImplicitFiles(std::vector<std::unique_ptr<File> > &) {
    return true;
  }

  /// \brief Finalize the symbol values
  void finalizeSymbolValues() {}

  /// \brief allocate Commons, some architectures may move small common
  /// symbols over to small data, this would also be used
  void allocateCommons() {}

  /// \brief create dynamic table
  LLD_UNIQUE_BUMP_PTR(DynamicTable<ELFT>) createDynamicTable() {
    return LLD_UNIQUE_BUMP_PTR(DynamicTable<ELFT>)(
        new (_alloc) DynamicTable<ELFT>(
            this->_context, ".dynamic", DefaultLayout<ELFT>::ORDER_DYNAMIC));
  }

  /// \brief create dynamic symbol table
  LLD_UNIQUE_BUMP_PTR(DynamicSymbolTable<ELFT>) createDynamicSymbolTable() {
    return LLD_UNIQUE_BUMP_PTR(DynamicSymbolTable<ELFT>)(
        new (_alloc) DynamicSymbolTable<ELFT>(
            this->_context, ".dynsym",
            DefaultLayout<ELFT>::ORDER_DYNAMIC_SYMBOLS));
  }

  virtual std::unique_ptr<Reader> getObjReader(bool atomizeStrings) {
    return std::unique_ptr<Reader>(new ELFObjectReader(atomizeStrings));
  }

  virtual std::unique_ptr<Reader> getDSOReader(bool useShlibUndefines) {
    return std::unique_ptr<Reader>(new ELFDSOReader(useShlibUndefines));
  }

protected:
  llvm::BumpPtrAllocator _alloc;
};
} // end namespace elf
} // end namespace lld

#endif
