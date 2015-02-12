//===- lib/ReaderWriter/ELF/HexagonELFReader.h ----------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_HEXAGON_ELF_READER_H
#define LLD_READER_WRITER_HEXAGON_ELF_READER_H

#include "ELFReader.h"
#include "HexagonELFFile.h"

namespace lld {
namespace elf {

typedef llvm::object::ELFType<llvm::support::little, 2, false> HexagonELFType;

struct HexagonDynamicFileCreateELFTraits {
  typedef llvm::ErrorOr<std::unique_ptr<lld::SharedLibraryFile>> result_type;

  template <class ELFT>
  static result_type create(std::unique_ptr<llvm::MemoryBuffer> mb,
                            HexagonLinkingContext &ctx) {
    return lld::elf::HexagonDynamicFile<ELFT>::create(std::move(mb), ctx);
  }
};

struct HexagonELFFileCreateELFTraits {
  typedef llvm::ErrorOr<std::unique_ptr<lld::File>> result_type;

  template <class ELFT>
  static result_type create(std::unique_ptr<llvm::MemoryBuffer> mb,
                            HexagonLinkingContext &ctx) {
    return lld::elf::HexagonELFFile<ELFT>::create(std::move(mb), ctx);
  }
};

class HexagonELFObjectReader
    : public ELFObjectReader<HexagonELFType, HexagonELFFileCreateELFTraits,
                             HexagonLinkingContext> {
public:
  HexagonELFObjectReader(HexagonLinkingContext &ctx)
      : ELFObjectReader<HexagonELFType, HexagonELFFileCreateELFTraits,
                        HexagonLinkingContext>(ctx, llvm::ELF::EM_HEXAGON) {}
};

class HexagonELFDSOReader
    : public ELFDSOReader<HexagonELFType, HexagonDynamicFileCreateELFTraits,
                          HexagonLinkingContext> {
public:
  HexagonELFDSOReader(HexagonLinkingContext &ctx)
      : ELFDSOReader<HexagonELFType, HexagonDynamicFileCreateELFTraits,
                     HexagonLinkingContext>(ctx, llvm::ELF::EM_HEXAGON) {}
};

} // namespace elf
} // namespace lld

#endif // LLD_READER_WRITER_ELF_READER_H
