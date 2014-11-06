//===- lib/ReaderWriter/ELF/MipsELFFlagsMerger.h --------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef LLD_READER_WRITER_ELF_MIPS_MIPS_ELF_FLAGS_MERGER_H
#define LLD_READER_WRITER_ELF_MIPS_MIPS_ELF_FLAGS_MERGER_H

#include <mutex>
#include <system_error>

namespace lld {
namespace elf {

class MipsELFFlagsMerger {
public:
  MipsELFFlagsMerger();

  uint32_t getMergedELFFlags() const;

  /// \brief Merge saved ELF header flags and the new set of flags.
  std::error_code merge(uint8_t newClass, uint32_t newFlags);

private:
  std::mutex _mutex;
  uint32_t _flags;
};

} // namespace elf
} // namespace lld

#endif
