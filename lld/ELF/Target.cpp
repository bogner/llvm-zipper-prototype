//===- Target.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Target.h"
#include "Error.h"
#include "Symbols.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ELF.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;

namespace lld {
namespace elf2 {

std::unique_ptr<TargetInfo> Target;

TargetInfo::~TargetInfo() {}

bool TargetInfo::relocPointsToGot(uint32_t Type) const { return false; }

X86TargetInfo::X86TargetInfo() {
  PCRelReloc = R_386_PC32;
  GotReloc = R_386_GLOB_DAT;
  GotRefReloc = R_386_GOT32;
}

void X86TargetInfo::writePltEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                                  uint64_t PltEntryAddr) const {
  // jmpl *val; nop; nop
  const uint8_t Inst[] = {0xff, 0x25, 0, 0, 0, 0, 0x90, 0x90};
  memcpy(Buf, Inst, sizeof(Inst));
  assert(isUInt<32>(GotEntryAddr));
  write32le(Buf + 2, GotEntryAddr);
}

bool X86TargetInfo::relocNeedsGot(uint32_t Type, const SymbolBody &S) const {
  if (relocNeedsPlt(Type, S))
    return true;
  switch (Type) {
  default:
    return false;
  case R_386_GOT32:
    return true;
  }
}

bool X86TargetInfo::relocPointsToGot(uint32_t Type) const {
  return Type == R_386_GOTPC;
}

bool X86TargetInfo::relocNeedsPlt(uint32_t Type, const SymbolBody &S) const {
  switch (Type) {
  default:
    return false;
  case R_386_PLT32:
    return true;
  }
}

static void add32le(uint8_t *P, int32_t V) { write32le(P, read32le(P) + V); }

void X86TargetInfo::relocateOne(uint8_t *Buf, const void *RelP, uint32_t Type,
                                uint64_t BaseAddr, uint64_t SymVA,
                                uint64_t GotVA) const {
  typedef ELFFile<ELF32LE>::Elf_Rel Elf_Rel;
  auto &Rel = *reinterpret_cast<const Elf_Rel *>(RelP);

  uint32_t Offset = Rel.r_offset;
  uint8_t *Location = Buf + Offset;
  switch (Type) {
  case R_386_GOT32:
    add32le(Location, SymVA - GotVA);
    break;
  case R_386_PC32:
    add32le(Location, SymVA - (BaseAddr + Offset));
    break;
  case R_386_32:
    add32le(Location, SymVA);
    break;
  default:
    error(Twine("unrecognized reloc ") + Twine(Type));
    break;
  }
}

X86_64TargetInfo::X86_64TargetInfo() {
  PCRelReloc = R_X86_64_PC32;
  GotReloc = R_X86_64_GLOB_DAT;
  GotRefReloc = R_X86_64_PC32;
}

void X86_64TargetInfo::writePltEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                                     uint64_t PltEntryAddr) const {
  // jmpq *val(%rip); nop; nop
  const uint8_t Inst[] = {0xff, 0x25, 0, 0, 0, 0, 0x90, 0x90};
  memcpy(Buf, Inst, sizeof(Inst));

  uintptr_t NextPC = PltEntryAddr + 6;
  intptr_t Delta = GotEntryAddr - NextPC;
  assert(isInt<32>(Delta));
  write32le(Buf + 2, Delta);
}

bool X86_64TargetInfo::relocNeedsGot(uint32_t Type, const SymbolBody &S) const {
  if (relocNeedsPlt(Type, S))
    return true;
  switch (Type) {
  default:
    return false;
  case R_X86_64_GOTPCREL:
    return true;
  }
}

bool X86_64TargetInfo::relocNeedsPlt(uint32_t Type, const SymbolBody &S) const {
  switch (Type) {
  default:
    return false;
  case R_X86_64_PC32:
    // This relocation is defined to have a value of (S + A - P).
    // The problems start when a non PIC program calls a function is a shared
    // library.
    // In an idea world, we could just report an error saying the relocation
    // can overflow at runtime.
    // In the real world, crt1.o has a R_X86_64_PC32 pointing to libc.so.
    // The general idea is to create a PLT entry and use that as the function
    // value, which is why we return true in here.
    // The remaining (unimplemented) problem is making sure pointer equality
    // still works. For that, we need the help of the dynamic linker. We
    // let it know that we have a direct reference to a symbol by creating
    // an undefined symbol with a non zero st_value. Seeing that the
    // dynamic linker resolves the symbol to the value of the symbol we created.
    // This is true even for got entries, so pointer equality is maintained.
    // To avoid an infinite loop, the only entry that points to the
    // real function is a dedicated got entry used by the plt.
    return S.isShared();
  case R_X86_64_PLT32:
    return true;
  }
}

void X86_64TargetInfo::relocateOne(uint8_t *Buf, const void *RelP,
                                   uint32_t Type, uint64_t BaseAddr,
                                   uint64_t SymVA, uint64_t GotVA) const {
  typedef ELFFile<ELF64LE>::Elf_Rela Elf_Rela;
  auto &Rel = *reinterpret_cast<const Elf_Rela *>(RelP);

  uint64_t Offset = Rel.r_offset;
  uint8_t *Location = Buf + Offset;
  switch (Type) {
  case R_X86_64_PC32:
  case R_X86_64_GOTPCREL:
    write32le(Location, SymVA + Rel.r_addend - (BaseAddr + Offset));
    break;
  case R_X86_64_64:
    write64le(Location, SymVA + Rel.r_addend);
    break;
  case R_X86_64_32: {
  case R_X86_64_32S:
    uint64_t VA = SymVA + Rel.r_addend;
    if (Type == R_X86_64_32 && !isUInt<32>(VA))
      error("R_X86_64_32 out of range");
    else if (!isInt<32>(VA))
      error("R_X86_64_32S out of range");

    write32le(Location, VA);
    break;
  }
  default:
    error(Twine("unrecognized reloc ") + Twine(Type));
    break;
  }
}

PPC64TargetInfo::PPC64TargetInfo() {
  // PCRelReloc = FIXME
  // GotReloc = FIXME
}
void PPC64TargetInfo::writePltEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                                    uint64_t PltEntryAddr) const {}
bool PPC64TargetInfo::relocNeedsGot(uint32_t Type, const SymbolBody &S) const {
  return false;
}
bool PPC64TargetInfo::relocNeedsPlt(uint32_t Type, const SymbolBody &S) const {
  return false;
}
void PPC64TargetInfo::relocateOne(uint8_t *Buf, const void *RelP, uint32_t Type,
                                  uint64_t BaseAddr, uint64_t SymVA,
                                  uint64_t GotVA) const {
  typedef ELFFile<ELF64BE>::Elf_Rela Elf_Rela;
  auto &Rel = *reinterpret_cast<const Elf_Rela *>(RelP);

  uint64_t Offset = Rel.r_offset;
  uint8_t *Location = Buf + Offset;
  switch (Type) {
  case R_PPC64_ADDR64:
    write64be(Location, SymVA + Rel.r_addend);
    break;
  case R_PPC64_TOC:
    // We don't create a TOC yet.
    break;
  default:
    error(Twine("unrecognized reloc ") + Twine(Type));
    break;
  }
}

PPCTargetInfo::PPCTargetInfo() {
  // PCRelReloc = FIXME
  // GotReloc = FIXME
}
void PPCTargetInfo::writePltEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                                  uint64_t PltEntryAddr) const {}
bool PPCTargetInfo::relocNeedsGot(uint32_t Type, const SymbolBody &S) const {
  return false;
}
bool PPCTargetInfo::relocNeedsPlt(uint32_t Type, const SymbolBody &S) const {
  return false;
}
void PPCTargetInfo::relocateOne(uint8_t *Buf, const void *RelP, uint32_t Type,
                                uint64_t BaseAddr, uint64_t SymVA,
                                uint64_t GotVA) const {}

ARMTargetInfo::ARMTargetInfo() {
  // PCRelReloc = FIXME
  // GotReloc = FIXME
}
void ARMTargetInfo::writePltEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                                  uint64_t PltEntryAddr) const {}
bool ARMTargetInfo::relocNeedsGot(uint32_t Type, const SymbolBody &S) const {
  return false;
}
bool ARMTargetInfo::relocNeedsPlt(uint32_t Type, const SymbolBody &S) const {
  return false;
}
void ARMTargetInfo::relocateOne(uint8_t *Buf, const void *RelP, uint32_t Type,
                                uint64_t BaseAddr, uint64_t SymVA,
                                uint64_t GotVA) const {}

AArch64TargetInfo::AArch64TargetInfo() {
  // PCRelReloc = FIXME
  // GotReloc = FIXME
}
void AArch64TargetInfo::writePltEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                                      uint64_t PltEntryAddr) const {}
bool AArch64TargetInfo::relocNeedsGot(uint32_t Type,
                                      const SymbolBody &S) const {
  return false;
}
bool AArch64TargetInfo::relocNeedsPlt(uint32_t Type,
                                      const SymbolBody &S) const {
  return false;
}

static void handle_ADR_PREL_LO21(uint8_t *Location, uint64_t S, int64_t A,
                                 uint64_t P) {
  uint64_t X = S + A - P;
  if (!isInt<21>(X))
    error("Relocation R_AARCH64_ADR_PREL_LO21 out of range");
  uint32_t Imm = X & 0x1FFFFF;
  uint32_t ImmLo = (Imm & 0x3) << 29;
  uint32_t ImmHi = ((Imm & 0x1FFFFC) >> 2) << 5;
  uint64_t Mask = (0x3 << 29) | (0x7FFFF << 5);
  write32le(Location, (read32le(Location) & ~Mask) | ImmLo | ImmHi);
}

void AArch64TargetInfo::relocateOne(uint8_t *Buf, const void *RelP,
                                    uint32_t Type, uint64_t BaseAddr,
                                    uint64_t SymVA, uint64_t GotVA) const {
  typedef ELFFile<ELF64LE>::Elf_Rela Elf_Rela;
  auto &Rel = *reinterpret_cast<const Elf_Rela *>(RelP);

  uint8_t *Location = Buf + Rel.r_offset;
  uint64_t S = SymVA;
  int64_t A = Rel.r_addend;
  uint64_t P = BaseAddr + Rel.r_offset;
  switch (Type) {
  case R_AARCH64_ADR_PREL_LO21:
    handle_ADR_PREL_LO21(Location, S, A, P);
    break;
  default:
    error(Twine("unrecognized reloc ") + Twine(Type));
    break;
  }
}

MipsTargetInfo::MipsTargetInfo() {
  // PCRelReloc = FIXME
  // GotReloc = FIXME
  DefaultEntry = "__start";
}

void MipsTargetInfo::writePltEntry(uint8_t *Buf, uint64_t GotEntryAddr,
                                   uint64_t PltEntryAddr) const {}

bool MipsTargetInfo::relocNeedsGot(uint32_t Type, const SymbolBody &S) const {
  return false;
}

bool MipsTargetInfo::relocNeedsPlt(uint32_t Type, const SymbolBody &S) const {
  return false;
}

void MipsTargetInfo::relocateOne(uint8_t *Buf, const void *RelP, uint32_t Type,
                                 uint64_t BaseAddr, uint64_t SymVA,
                                 uint64_t GotVA) const {}
}
}
