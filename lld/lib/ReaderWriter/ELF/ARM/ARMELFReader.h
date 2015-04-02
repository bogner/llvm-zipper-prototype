//===--------- lib/ReaderWriter/ELF/ARM/ARMELFReader.h --------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ARM_ARM_ELF_READER_H
#define LLD_READER_WRITER_ARM_ARM_ELF_READER_H

#include "ARMELFFile.h"
#include "ELFReader.h"

namespace lld {
namespace elf {

typedef llvm::object::ELFType<llvm::support::little, 2, false> ARMELFType;

struct ARMELFFileCreateELFTraits {
  template <class ELFT>
  static llvm::ErrorOr<std::unique_ptr<lld::File>>
  create(std::unique_ptr<llvm::MemoryBuffer> mb, ARMLinkingContext &ctx) {
    return lld::elf::ARMELFFile<ELFT>::create(std::move(mb), ctx);
  }
};

typedef ELFObjectReader<ARMELFType, ARMELFFileCreateELFTraits,
                        ARMLinkingContext> ARMELFObjectReader;
typedef ELFDSOReader<ARMELFType, ARMLinkingContext> ARMELFDSOReader;

} // namespace elf
} // namespace lld

#endif // LLD_READER_WRITER_ARM_ARM_ELF_READER_H
