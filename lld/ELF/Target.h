//===- Target.h -------------------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_TARGET_H
#define LLD_ELF_TARGET_H

#include "llvm/ADT/StringRef.h"

#include <memory>

namespace lld {
namespace elf2 {
class SymbolBody;

class TargetInfo {
public:
  unsigned getPageSize() const { return PageSize; }
  uint64_t getVAStart() const { return VAStart; }
  unsigned getPCRelReloc() const { return PCRelReloc; }
  unsigned getGotReloc() const { return GotReloc; }
  unsigned getPltReloc() const { return PltReloc; }
  unsigned getGotRefReloc() const { return GotRefReloc; }
  unsigned getRelativeReloc() const { return RelativeReloc; }
  unsigned getPltZeroEntrySize() const { return PltZeroEntrySize; }
  unsigned getPltEntrySize() const { return PltEntrySize; }
  bool supportsLazyRelocations() const { return LazyRelocations; }
  virtual unsigned getPLTRefReloc(unsigned Type) const;
  virtual void writeGotPltEntry(uint8_t *Buf, uint64_t Plt) const = 0;
  virtual void writePltZeroEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                                 uint64_t PltEntryAddr) const = 0;
  virtual void writePltEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                             uint64_t PltEntryAddr, int32_t Index) const = 0;
  virtual bool isRelRelative(uint32_t Type) const;
  virtual bool relocNeedsGot(uint32_t Type, const SymbolBody &S) const = 0;
  virtual bool relocPointsToGot(uint32_t Type) const;
  virtual bool relocNeedsPlt(uint32_t Type, const SymbolBody &S) const = 0;
  virtual void relocateOne(uint8_t *Buf, uint8_t *BufEnd, const void *RelP,
                           uint32_t Type, uint64_t BaseAddr,
                           uint64_t SymVA) const = 0;

  virtual ~TargetInfo();

protected:
  unsigned PageSize = 4096;

  // On freebsd x86_64 the first page cannot be mmaped.
  // On linux that is controled by vm.mmap_min_addr. At least on some x86_64
  // installs that is 65536, so the first 15 pages cannot be used.
  // Given that, the smallest value that can be used in here is 0x10000.
  // If using 2MB pages, the smallest page aligned address that works is
  // 0x200000, but it looks like every OS uses 4k pages for executables.
  uint64_t VAStart = 0x10000;

  unsigned PCRelReloc;
  unsigned GotRefReloc;
  unsigned GotReloc;
  unsigned PltReloc;
  unsigned RelativeReloc;
  unsigned PltEntrySize = 8;
  unsigned PltZeroEntrySize = 0;
  bool LazyRelocations = false;
};

uint64_t getPPC64TocBase();

extern std::unique_ptr<TargetInfo> Target;
TargetInfo *createTarget();
}
}

#endif
