//===- lib/ReaderWriter/ELF/ELFReader.h -----------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_READER_H
#define LLD_READER_WRITER_ELF_READER_H

#include "CreateELF.h"
#include "DynamicFile.h"
#include "ELFFile.h"
#include "lld/Core/Reader.h"

namespace lld {
namespace elf {

template <typename ELFT, typename ELFTraitsT, typename ContextT>
class ELFObjectReader : public Reader {
public:
  typedef llvm::object::Elf_Ehdr_Impl<ELFT> Elf_Ehdr;

  ELFObjectReader(ContextT &ctx, uint64_t machine)
      : _ctx(ctx), _machine(machine) {}

  bool canParse(file_magic magic, StringRef,
                const MemoryBuffer &buf) const override {
    return (magic == llvm::sys::fs::file_magic::elf_relocatable &&
            elfHeader(buf)->e_machine == _machine);
  }

  std::error_code
  loadFile(std::unique_ptr<MemoryBuffer> mb, const class Registry &,
           std::vector<std::unique_ptr<File>> &result) const override {
    std::size_t maxAlignment =
        1ULL << llvm::countTrailingZeros(uintptr_t(mb->getBufferStart()));
    auto f =
        createELF<ELFTraitsT>(llvm::object::getElfArchType(mb->getBuffer()),
                              maxAlignment, std::move(mb), _ctx);
    if (std::error_code ec = f.getError())
      return ec;
    result.push_back(std::move(*f));
    return std::error_code();
  }

  const Elf_Ehdr *elfHeader(const MemoryBuffer &buf) const {
    const uint8_t *data =
        reinterpret_cast<const uint8_t *>(buf.getBuffer().data());
    return (reinterpret_cast<const Elf_Ehdr *>(data));
  }

protected:
  ContextT &_ctx;
  uint64_t _machine;
};

struct DynamicFileCreateELFTraits {
  typedef llvm::ErrorOr<std::unique_ptr<lld::SharedLibraryFile>> result_type;

  template<typename ELFT, typename ContextT>
  static result_type create(std::unique_ptr<llvm::MemoryBuffer> mb,
                            ContextT &ctx) {
    return DynamicFile<ELFT>::create(std::move(mb), ctx);
  }
};

template <typename ELFT, typename ContextT>
class ELFDSOReader : public Reader {
public:
  typedef llvm::object::Elf_Ehdr_Impl<ELFT> Elf_Ehdr;

  ELFDSOReader(ContextT &ctx, uint64_t machine)
      : _ctx(ctx), _machine(machine) {}

  bool canParse(file_magic magic, StringRef,
                const MemoryBuffer &buf) const override {
    return (magic == llvm::sys::fs::file_magic::elf_shared_object &&
            elfHeader(buf)->e_machine == _machine);
  }

  std::error_code
  loadFile(std::unique_ptr<MemoryBuffer> mb, const class Registry &,
           std::vector<std::unique_ptr<File>> &result) const override {
    std::size_t maxAlignment =
        1ULL << llvm::countTrailingZeros(uintptr_t(mb->getBufferStart()));
    auto f = createELF<DynamicFileCreateELFTraits>(
        llvm::object::getElfArchType(mb->getBuffer()),
        maxAlignment, std::move(mb), _ctx);
    if (std::error_code ec = f.getError())
      return ec;
    result.push_back(std::move(*f));
    return std::error_code();
  }

  const Elf_Ehdr *elfHeader(const MemoryBuffer &buf) const {
    const uint8_t *data =
        reinterpret_cast<const uint8_t *>(buf.getBuffer().data());
    return (reinterpret_cast<const Elf_Ehdr *>(data));
  }

protected:
  ContextT &_ctx;
  uint64_t _machine;
};

} // namespace elf
} // namespace lld

#endif // LLD_READER_WRITER_ELF_READER_H
