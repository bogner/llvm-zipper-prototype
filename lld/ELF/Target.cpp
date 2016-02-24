//===- Target.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Machine-specific things, such as applying relocations, creation of
// GOT or PLT entries, etc., are handled in this file.
//
// Refer the ELF spec for the single letter varaibles, S, A or P, used
// in this file. SA is S+A.
//
//===----------------------------------------------------------------------===//

#include "Target.h"
#include "Error.h"
#include "OutputSections.h"
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

TargetInfo *Target;

template <endianness E> static void add32(void *P, int32_t V) {
  write32<E>(P, read32<E>(P) + V);
}

static void add32le(uint8_t *P, int32_t V) { add32<support::little>(P, V); }
static void or32le(uint8_t *P, int32_t V) { write32le(P, read32le(P) | V); }

template <unsigned N> static void checkInt(int64_t V, uint32_t Type) {
  if (isInt<N>(V))
    return;
  StringRef S = getELFRelocationTypeName(Config->EMachine, Type);
  error("Relocation " + S + " out of range");
}

template <unsigned N> static void checkUInt(uint64_t V, uint32_t Type) {
  if (isUInt<N>(V))
    return;
  StringRef S = getELFRelocationTypeName(Config->EMachine, Type);
  error("Relocation " + S + " out of range");
}

template <unsigned N> static void checkIntUInt(uint64_t V, uint32_t Type) {
  if (isInt<N>(V) || isUInt<N>(V))
    return;
  StringRef S = getELFRelocationTypeName(Config->EMachine, Type);
  error("Relocation " + S + " out of range");
}

template <unsigned N> static void checkAlignment(uint64_t V, uint32_t Type) {
  if ((V & (N - 1)) == 0)
    return;
  StringRef S = getELFRelocationTypeName(Config->EMachine, Type);
  error("Improper alignment for relocation " + S);
}

template <class ELFT> bool isGnuIFunc(const SymbolBody &S) {
  if (auto *SS = dyn_cast<DefinedElf<ELFT>>(&S))
    return SS->Sym.getType() == STT_GNU_IFUNC;
  return false;
}

namespace {
class X86TargetInfo final : public TargetInfo {
public:
  X86TargetInfo();
  void writeGotPltHeader(uint8_t *Buf) const override;
  unsigned getDynRel(unsigned Type) const override;
  unsigned getTlsGotRel(unsigned Type) const override;
  bool isTlsLocalDynamicRel(unsigned Type) const override;
  bool isTlsGlobalDynamicRel(unsigned Type) const override;
  bool isTlsDynRel(unsigned Type, const SymbolBody &S) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  bool needsCopyRelImpl(uint32_t Type) const override;
  bool needsDynRelative(unsigned Type) const override;
  bool needsGot(uint32_t Type, SymbolBody &S) const override;
  PltNeed needsPlt(uint32_t Type, const SymbolBody &S) const override;
  void relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                   uint64_t SA, uint64_t ZA = 0,
                   uint8_t *PairedLoc = nullptr) const override;
  bool canRelaxTls(unsigned Type, const SymbolBody *S) const override;
  unsigned relaxTls(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                    uint64_t SA, const SymbolBody *S) const override;
  bool isGotRelative(uint32_t Type) const override;

private:
  void relocateTlsLdToLe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                         uint64_t SA) const;
  void relocateTlsGdToIe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                         uint64_t SA) const;
  void relocateTlsGdToLe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                         uint64_t SA) const;
  void relocateTlsIeToLe(unsigned Type, uint8_t *Loc, uint8_t *BufEnd,
                         uint64_t P, uint64_t SA) const;
};

class X86_64TargetInfo final : public TargetInfo {
public:
  X86_64TargetInfo();
  unsigned getTlsGotRel(unsigned Type) const override;
  bool isTlsLocalDynamicRel(unsigned Type) const override;
  bool isTlsGlobalDynamicRel(unsigned Type) const override;
  bool isTlsDynRel(unsigned Type, const SymbolBody &S) const override;
  void writeGotPltHeader(uint8_t *Buf) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  bool needsCopyRelImpl(uint32_t Type) const override;
  bool needsGot(uint32_t Type, SymbolBody &S) const override;
  PltNeed needsPlt(uint32_t Type, const SymbolBody &S) const override;
  void relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                   uint64_t SA, uint64_t ZA = 0,
                   uint8_t *PairedLoc = nullptr) const override;
  bool isRelRelative(uint32_t Type) const override;
  bool canRelaxTls(unsigned Type, const SymbolBody *S) const override;
  bool isSizeRel(uint32_t Type) const override;
  unsigned relaxTls(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                    uint64_t SA, const SymbolBody *S) const override;

private:
  void relocateTlsLdToLe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                         uint64_t SA) const;
  void relocateTlsGdToLe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                         uint64_t SA) const;
  void relocateTlsGdToIe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                         uint64_t SA) const;
  void relocateTlsIeToLe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                         uint64_t SA) const;
};

class PPCTargetInfo final : public TargetInfo {
public:
  PPCTargetInfo();
  void relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                   uint64_t SA, uint64_t ZA = 0,
                   uint8_t *PairedLoc = nullptr) const override;
  bool isRelRelative(uint32_t Type) const override;
};

class PPC64TargetInfo final : public TargetInfo {
public:
  PPC64TargetInfo();
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  bool needsGot(uint32_t Type, SymbolBody &S) const override;
  PltNeed needsPlt(uint32_t Type, const SymbolBody &S) const override;
  void relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                   uint64_t SA, uint64_t ZA = 0,
                   uint8_t *PairedLoc = nullptr) const override;
  bool isRelRelative(uint32_t Type) const override;
};

class AArch64TargetInfo final : public TargetInfo {
public:
  AArch64TargetInfo();
  unsigned getDynRel(unsigned Type) const override;
  bool isTlsGlobalDynamicRel(unsigned Type) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  unsigned getTlsGotRel(unsigned Type) const override;
  bool isTlsDynRel(unsigned Type, const SymbolBody &S) const override;
  bool isRelRelative(uint32_t Type) const override;
  bool needsCopyRelImpl(uint32_t Type) const override;
  bool needsGot(uint32_t Type, SymbolBody &S) const override;
  PltNeed needsPlt(uint32_t Type, const SymbolBody &S) const override;
  void relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                   uint64_t SA, uint64_t ZA = 0,
                   uint8_t *PairedLoc = nullptr) const override;
  unsigned relaxTls(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                    uint64_t SA, const SymbolBody *S) const override;
  bool canRelaxTls(unsigned Type, const SymbolBody *S) const override;

private:
  void relocateTlsGdToLe(unsigned Type, uint8_t *Loc, uint8_t *BufEnd,
                         uint64_t P, uint64_t SA) const;
  void relocateTlsIeToLe(unsigned Type, uint8_t *Loc, uint8_t *BufEnd,
                         uint64_t P, uint64_t SA) const;

  static const uint64_t TcbSize = 16;
};

class AMDGPUTargetInfo final : public TargetInfo {
public:
  AMDGPUTargetInfo() {}
  void relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                   uint64_t SA, uint64_t ZA = 0,
                   uint8_t *PairedLoc = nullptr) const override;
};

template <class ELFT> class MipsTargetInfo final : public TargetInfo {
public:
  MipsTargetInfo();
  unsigned getDynRel(unsigned Type) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  void writeGotHeader(uint8_t *Buf) const override;
  bool needsCopyRelImpl(uint32_t Type) const override;
  bool needsGot(uint32_t Type, SymbolBody &S) const override;
  PltNeed needsPlt(uint32_t Type, const SymbolBody &S) const override;
  void relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type, uint64_t P,
                   uint64_t S, uint64_t ZA = 0,
                   uint8_t *PairedLoc = nullptr) const override;
  bool isHintRel(uint32_t Type) const override;
  bool isRelRelative(uint32_t Type) const override;
};
} // anonymous namespace

TargetInfo *createTarget() {
  switch (Config->EMachine) {
  case EM_386:
    return new X86TargetInfo();
  case EM_AARCH64:
    return new AArch64TargetInfo();
  case EM_AMDGPU:
    return new AMDGPUTargetInfo();
  case EM_MIPS:
    switch (Config->EKind) {
    case ELF32LEKind:
      return new MipsTargetInfo<ELF32LE>();
    case ELF32BEKind:
      return new MipsTargetInfo<ELF32BE>();
    default:
      fatal("Unsupported MIPS target");
    }
  case EM_PPC:
    return new PPCTargetInfo();
  case EM_PPC64:
    return new PPC64TargetInfo();
  case EM_X86_64:
    return new X86_64TargetInfo();
  }
  fatal("Unknown target machine");
}

TargetInfo::~TargetInfo() {}

bool TargetInfo::canRelaxTls(unsigned Type, const SymbolBody *S) const {
  return false;
}

uint64_t TargetInfo::getVAStart() const { return Config->Shared ? 0 : VAStart; }

bool TargetInfo::needsCopyRelImpl(uint32_t Type) const { return false; }

template <typename ELFT> static bool mayNeedCopy(const SymbolBody &S) {
  if (Config->Shared)
    return false;
  auto *SS = dyn_cast<SharedSymbol<ELFT>>(&S);
  if (!SS)
    return false;
  return SS->Sym.getType() == STT_OBJECT;
}

template <class ELFT>
bool TargetInfo::needsCopyRel(uint32_t Type, const SymbolBody &S) const {
  return mayNeedCopy<ELFT>(S) && needsCopyRelImpl(Type);
}

bool TargetInfo::isTlsDynRel(unsigned Type, const SymbolBody &S) const {
  return false;
}

bool TargetInfo::isGotRelative(uint32_t Type) const { return false; }
bool TargetInfo::isHintRel(uint32_t Type) const { return false; }
bool TargetInfo::isRelRelative(uint32_t Type) const { return true; }
bool TargetInfo::isSizeRel(uint32_t Type) const { return false; }

bool TargetInfo::needsGot(uint32_t Type, SymbolBody &S) const { return false; }

TargetInfo::PltNeed TargetInfo::needsPlt(uint32_t Type,
                                         const SymbolBody &S) const {
  return Plt_No;
}

bool TargetInfo::isTlsLocalDynamicRel(unsigned Type) const {
  return false;
}

bool TargetInfo::isTlsGlobalDynamicRel(unsigned Type) const {
  return false;
}

unsigned TargetInfo::relaxTls(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type,
                              uint64_t P, uint64_t SA,
                              const SymbolBody *S) const {
  return 0;
}

X86TargetInfo::X86TargetInfo() {
  CopyRel = R_386_COPY;
  GotRel = R_386_GLOB_DAT;
  PltRel = R_386_JUMP_SLOT;
  IRelativeRel = R_386_IRELATIVE;
  RelativeRel = R_386_RELATIVE;
  TlsGotRel = R_386_TLS_TPOFF;
  TlsModuleIndexRel = R_386_TLS_DTPMOD32;
  TlsOffsetRel = R_386_TLS_DTPOFF32;
  UseLazyBinding = true;
  PltEntrySize = 16;
  PltZeroSize = 16;
}

void X86TargetInfo::writeGotPltHeader(uint8_t *Buf) const {
  write32le(Buf, Out<ELF32LE>::Dynamic->getVA());
}

void X86TargetInfo::writeGotPlt(uint8_t *Buf, uint64_t Plt) const {
  // Entries in .got.plt initially points back to the corresponding
  // PLT entries with a fixed offset to skip the first instruction.
  write32le(Buf, Plt + 6);
}

unsigned X86TargetInfo::getDynRel(unsigned Type) const {
  if (Type == R_386_TLS_LE)
    return R_386_TLS_TPOFF;
  if (Type == R_386_TLS_LE_32)
    return R_386_TLS_TPOFF32;
  return Type;
}

unsigned X86TargetInfo::getTlsGotRel(unsigned Type) const {
  if (Type == R_386_TLS_IE)
    return Type;
  return TlsGotRel;
}

bool X86TargetInfo::isTlsGlobalDynamicRel(unsigned Type) const {
  return Type == R_386_TLS_GD;
}

bool X86TargetInfo::isTlsLocalDynamicRel(unsigned Type) const {
  return Type == R_386_TLS_LDM;
}

bool X86TargetInfo::isTlsDynRel(unsigned Type, const SymbolBody &S) const {
  if (Type == R_386_TLS_LE || Type == R_386_TLS_LE_32 ||
      Type == R_386_TLS_GOTIE)
    return Config->Shared;
  if (Type == R_386_TLS_IE)
    return canBePreempted(&S, true);
  return Type == R_386_TLS_GD;
}

void X86TargetInfo::writePltZero(uint8_t *Buf) const {
  // Executable files and shared object files have
  // separate procedure linkage tables.
  if (Config->Shared) {
    const uint8_t V[] = {
        0xff, 0xb3, 0x04, 0x00, 0x00, 0x00, // pushl 4(%ebx)
        0xff, 0xa3, 0x08, 0x00, 0x00, 0x00, // jmp   *8(%ebx)
        0x90, 0x90, 0x90, 0x90              // nop; nop; nop; nop
    };
    memcpy(Buf, V, sizeof(V));
    return;
  }

  const uint8_t PltData[] = {
      0xff, 0x35, 0x00, 0x00, 0x00, 0x00, // pushl (GOT+4)
      0xff, 0x25, 0x00, 0x00, 0x00, 0x00, // jmp   *(GOT+8)
      0x90, 0x90, 0x90, 0x90              // nop; nop; nop; nop
  };
  memcpy(Buf, PltData, sizeof(PltData));
  uint32_t Got = Out<ELF32LE>::GotPlt->getVA();
  write32le(Buf + 2, Got + 4);
  write32le(Buf + 8, Got + 8);
}

void X86TargetInfo::writePlt(uint8_t *Buf, uint64_t GotEntryAddr,
                             uint64_t PltEntryAddr, int32_t Index,
                             unsigned RelOff) const {
  const uint8_t Inst[] = {
      0xff, 0x00, 0x00, 0x00, 0x00, 0x00, // jmp *foo_in_GOT|*foo@GOT(%ebx)
      0x68, 0x00, 0x00, 0x00, 0x00,       // pushl $reloc_offset
      0xe9, 0x00, 0x00, 0x00, 0x00        // jmp .PLT0@PC
  };
  memcpy(Buf, Inst, sizeof(Inst));

  // jmp *foo@GOT(%ebx) or jmp *foo_in_GOT
  Buf[1] = Config->Shared ? 0xa3 : 0x25;
  uint32_t Got = UseLazyBinding ? Out<ELF32LE>::GotPlt->getVA()
                                : Out<ELF32LE>::Got->getVA();
  write32le(Buf + 2, Config->Shared ? GotEntryAddr - Got : GotEntryAddr);
  write32le(Buf + 7, RelOff);
  write32le(Buf + 12, -Index * PltEntrySize - PltZeroSize - 16);
}

bool X86TargetInfo::needsCopyRelImpl(uint32_t Type) const {
  return Type == R_386_32 || Type == R_386_16 || Type == R_386_8;
}

bool X86TargetInfo::needsGot(uint32_t Type, SymbolBody &S) const {
  if (S.IsTls && Type == R_386_TLS_GD)
    return Target->canRelaxTls(Type, &S) && canBePreempted(&S, true);
  if (Type == R_386_TLS_GOTIE || Type == R_386_TLS_IE)
    return !canRelaxTls(Type, &S);
  return Type == R_386_GOT32 || needsPlt(Type, S);
}

TargetInfo::PltNeed X86TargetInfo::needsPlt(uint32_t Type,
                                            const SymbolBody &S) const {
  if (isGnuIFunc<ELF32LE>(S) ||
      (Type == R_386_PLT32 && canBePreempted(&S, true)) ||
      (Type == R_386_PC32 && S.isShared()))
    return Plt_Explicit;
  return Plt_No;
}

bool X86TargetInfo::isGotRelative(uint32_t Type) const {
  // This relocation does not require got entry,
  // but it is relative to got and needs it to be created.
  // Here we request for that.
  return Type == R_386_GOTOFF;
}

void X86TargetInfo::relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type,
                                uint64_t P, uint64_t SA, uint64_t ZA,
                                uint8_t *PairedLoc) const {
  switch (Type) {
  case R_386_32:
    add32le(Loc, SA);
    break;
  case R_386_GOT32: {
    uint64_t V = SA - Out<ELF32LE>::Got->getVA() -
                 Out<ELF32LE>::Got->getNumEntries() * 4;
    checkInt<32>(V, Type);
    add32le(Loc, V);
    break;
  }
  case R_386_GOTOFF:
    add32le(Loc, SA - Out<ELF32LE>::Got->getVA());
    break;
  case R_386_GOTPC:
    add32le(Loc, SA + Out<ELF32LE>::Got->getVA() - P);
    break;
  case R_386_PC32:
  case R_386_PLT32:
    add32le(Loc, SA - P);
    break;
  case R_386_TLS_GD:
  case R_386_TLS_LDM:
  case R_386_TLS_TPOFF: {
    uint64_t V = SA - Out<ELF32LE>::Got->getVA() -
                 Out<ELF32LE>::Got->getNumEntries() * 4;
    checkInt<32>(V, Type);
    write32le(Loc, V);
    break;
  }
  case R_386_TLS_IE:
  case R_386_TLS_LDO_32:
    write32le(Loc, SA);
    break;
  case R_386_TLS_LE:
    write32le(Loc, SA - Out<ELF32LE>::TlsPhdr->p_memsz);
    break;
  case R_386_TLS_LE_32:
    write32le(Loc, Out<ELF32LE>::TlsPhdr->p_memsz - SA);
    break;
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

bool X86TargetInfo::canRelaxTls(unsigned Type, const SymbolBody *S) const {
  if (Config->Shared || (S && !S->IsTls))
    return false;
  return Type == R_386_TLS_LDO_32 || Type == R_386_TLS_LDM ||
         Type == R_386_TLS_GD ||
         (Type == R_386_TLS_IE && !canBePreempted(S, true)) ||
         (Type == R_386_TLS_GOTIE && !canBePreempted(S, true));
}

bool X86TargetInfo::needsDynRelative(unsigned Type) const {
  return Config->Shared && Type == R_386_TLS_IE;
}

unsigned X86TargetInfo::relaxTls(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type,
                                 uint64_t P, uint64_t SA,
                                 const SymbolBody *S) const {
  switch (Type) {
  case R_386_TLS_GD:
    if (canBePreempted(S, true))
      relocateTlsGdToIe(Loc, BufEnd, P, SA);
    else
      relocateTlsGdToLe(Loc, BufEnd, P, SA);
    // The next relocation should be against __tls_get_addr, so skip it
    return 1;
  case R_386_TLS_GOTIE:
  case R_386_TLS_IE:
    relocateTlsIeToLe(Type, Loc, BufEnd, P, SA);
    return 0;
  case R_386_TLS_LDM:
    relocateTlsLdToLe(Loc, BufEnd, P, SA);
    // The next relocation should be against __tls_get_addr, so skip it
    return 1;
  case R_386_TLS_LDO_32:
    relocateOne(Loc, BufEnd, R_386_TLS_LE, P, SA);
    return 0;
  }
  llvm_unreachable("Unknown TLS optimization");
}

// "Ulrich Drepper, ELF Handling For Thread-Local Storage" (5.1
// IA-32 Linker Optimizations, http://www.akkadia.org/drepper/tls.pdf) shows
// how GD can be optimized to IE:
//   leal x@tlsgd(, %ebx, 1),
//   call __tls_get_addr@plt
// Is converted to:
//   movl %gs:0, %eax
//   addl x@gotntpoff(%ebx), %eax
void X86TargetInfo::relocateTlsGdToIe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                                      uint64_t SA) const {
  const uint8_t Inst[] = {
      0x65, 0xa1, 0x00, 0x00, 0x00, 0x00, // movl %gs:0, %eax
      0x03, 0x83, 0x00, 0x00, 0x00, 0x00  // addl 0(%ebx), %eax
  };
  memcpy(Loc - 3, Inst, sizeof(Inst));
  relocateOne(Loc + 5, BufEnd, R_386_32, P,
              SA - Out<ELF32LE>::Got->getVA() -
                  Out<ELF32LE>::Got->getNumEntries() * 4);
}

// GD can be optimized to LE:
//   leal x@tlsgd(, %ebx, 1),
//   call __tls_get_addr@plt
// Can be converted to:
//   movl %gs:0,%eax
//   addl $x@ntpoff,%eax
// But gold emits subl $foo@tpoff,%eax instead of addl.
// These instructions are completely equal in behavior.
// This method generates subl to be consistent with gold.
void X86TargetInfo::relocateTlsGdToLe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                                      uint64_t SA) const {
  const uint8_t Inst[] = {
      0x65, 0xa1, 0x00, 0x00, 0x00, 0x00, // movl %gs:0, %eax
      0x81, 0xe8, 0x00, 0x00, 0x00, 0x00  // subl 0(%ebx), %eax
  };
  memcpy(Loc - 3, Inst, sizeof(Inst));
  relocateOne(Loc + 5, BufEnd, R_386_32, P,
              Out<ELF32LE>::TlsPhdr->p_memsz - SA);
}

// LD can be optimized to LE:
//   leal foo(%reg),%eax
//   call ___tls_get_addr
// Is converted to:
//   movl %gs:0,%eax
//   nop
//   leal 0(%esi,1),%esi
void X86TargetInfo::relocateTlsLdToLe(uint8_t *Loc, uint8_t *BufEnd, uint64_t P,
                                      uint64_t SA) const {
  const uint8_t Inst[] = {
      0x65, 0xa1, 0x00, 0x00, 0x00, 0x00, // movl %gs:0,%eax
      0x90,                               // nop
      0x8d, 0x74, 0x26, 0x00              // leal 0(%esi,1),%esi
  };
  memcpy(Loc - 2, Inst, sizeof(Inst));
}

// In some conditions, relocations can be optimized to avoid using GOT.
// This function does that for Initial Exec to Local Exec case.
// Read "ELF Handling For Thread-Local Storage, 5.1
// IA-32 Linker Optimizations" (http://www.akkadia.org/drepper/tls.pdf)
// by Ulrich Drepper for details.
void X86TargetInfo::relocateTlsIeToLe(unsigned Type, uint8_t *Loc,
                                      uint8_t *BufEnd, uint64_t P,
                                      uint64_t SA) const {
  // Ulrich's document section 6.2 says that @gotntpoff can
  // be used with MOVL or ADDL instructions.
  // @indntpoff is similar to @gotntpoff, but for use in
  // position dependent code.
  uint8_t *Inst = Loc - 2;
  uint8_t *Op = Loc - 1;
  uint8_t Reg = (Loc[-1] >> 3) & 7;
  bool IsMov = *Inst == 0x8b;
  if (Type == R_386_TLS_IE) {
    // For R_386_TLS_IE relocation we perform the next transformations:
    // MOVL foo@INDNTPOFF,%EAX is transformed to MOVL $foo,%EAX
    // MOVL foo@INDNTPOFF,%REG is transformed to MOVL $foo,%REG
    // ADDL foo@INDNTPOFF,%REG is transformed to ADDL $foo,%REG
    // First one is special because when EAX is used the sequence is 5 bytes
    // long, otherwise it is 6 bytes.
    if (*Op == 0xa1) {
      *Op = 0xb8;
    } else {
      *Inst = IsMov ? 0xc7 : 0x81;
      *Op = 0xc0 | ((*Op >> 3) & 7);
    }
  } else {
    // R_386_TLS_GOTIE relocation can be optimized to
    // R_386_TLS_LE so that it does not use GOT.
    // "MOVL foo@GOTTPOFF(%RIP), %REG" is transformed to "MOVL $foo, %REG".
    // "ADDL foo@GOTNTPOFF(%RIP), %REG" is transformed to "LEAL foo(%REG), %REG"
    // Note: gold converts to ADDL instead of LEAL.
    *Inst = IsMov ? 0xc7 : 0x8d;
    if (IsMov)
      *Op = 0xc0 | ((*Op >> 3) & 7);
    else
      *Op = 0x80 | Reg | (Reg << 3);
  }
  relocateOne(Loc, BufEnd, R_386_TLS_LE, P, SA);
}

X86_64TargetInfo::X86_64TargetInfo() {
  CopyRel = R_X86_64_COPY;
  GotRel = R_X86_64_GLOB_DAT;
  PltRel = R_X86_64_JUMP_SLOT;
  RelativeRel = R_X86_64_RELATIVE;
  IRelativeRel = R_X86_64_IRELATIVE;
  TlsGotRel = R_X86_64_TPOFF64;
  TlsModuleIndexRel = R_X86_64_DTPMOD64;
  TlsOffsetRel = R_X86_64_DTPOFF64;
  UseLazyBinding = true;
  PltEntrySize = 16;
  PltZeroSize = 16;
}

void X86_64TargetInfo::writeGotPltHeader(uint8_t *Buf) const {
  write64le(Buf, Out<ELF64LE>::Dynamic->getVA());
}

void X86_64TargetInfo::writeGotPlt(uint8_t *Buf, uint64_t Plt) const {
  // See comments in X86TargetInfo::writeGotPlt.
  write32le(Buf, Plt + 6);
}

void X86_64TargetInfo::writePltZero(uint8_t *Buf) const {
  const uint8_t PltData[] = {
      0xff, 0x35, 0x00, 0x00, 0x00, 0x00, // pushq GOT+8(%rip)
      0xff, 0x25, 0x00, 0x00, 0x00, 0x00, // jmp *GOT+16(%rip)
      0x0f, 0x1f, 0x40, 0x00              // nopl 0x0(rax)
  };
  memcpy(Buf, PltData, sizeof(PltData));
  uint64_t Got = Out<ELF64LE>::GotPlt->getVA();
  uint64_t Plt = Out<ELF64LE>::Plt->getVA();
  write32le(Buf + 2, Got - Plt + 2); // GOT+8
  write32le(Buf + 8, Got - Plt + 4); // GOT+16
}

void X86_64TargetInfo::writePlt(uint8_t *Buf, uint64_t GotEntryAddr,
                                uint64_t PltEntryAddr, int32_t Index,
                                unsigned RelOff) const {
  const uint8_t Inst[] = {
      0xff, 0x25, 0x00, 0x00, 0x00, 0x00, // jmpq *got(%rip)
      0x68, 0x00, 0x00, 0x00, 0x00,       // pushq <relocation index>
      0xe9, 0x00, 0x00, 0x00, 0x00        // jmpq plt[0]
  };
  memcpy(Buf, Inst, sizeof(Inst));

  write32le(Buf + 2, GotEntryAddr - PltEntryAddr - 6);
  write32le(Buf + 7, Index);
  write32le(Buf + 12, -Index * PltEntrySize - PltZeroSize - 16);
}

bool X86_64TargetInfo::needsCopyRelImpl(uint32_t Type) const {
  return Type == R_X86_64_32S || Type == R_X86_64_32 || Type == R_X86_64_PC32 ||
         Type == R_X86_64_64;
}

bool X86_64TargetInfo::needsGot(uint32_t Type, SymbolBody &S) const {
  if (Type == R_X86_64_TLSGD)
    return Target->canRelaxTls(Type, &S) && canBePreempted(&S, true);
  if (Type == R_X86_64_GOTTPOFF)
    return !canRelaxTls(Type, &S);
  return Type == R_X86_64_GOTPCREL || needsPlt(Type, S);
}

unsigned X86_64TargetInfo::getTlsGotRel(unsigned Type) const {
  // No other types of TLS relocations requiring GOT should
  // reach here.
  assert(Type == R_X86_64_GOTTPOFF);
  return R_X86_64_PC32;
}

bool X86_64TargetInfo::isTlsGlobalDynamicRel(unsigned Type) const {
  return Type == R_X86_64_TLSGD;
}

bool X86_64TargetInfo::isTlsLocalDynamicRel(unsigned Type) const {
  return Type == R_X86_64_TLSLD;
}

bool X86_64TargetInfo::isTlsDynRel(unsigned Type, const SymbolBody &S) const {
  return Type == R_X86_64_GOTTPOFF || Type == R_X86_64_TLSGD;
}

TargetInfo::PltNeed X86_64TargetInfo::needsPlt(uint32_t Type,
                                               const SymbolBody &S) const {
  if (needsCopyRel<ELF64LE>(Type, S))
    return Plt_No;
  if (isGnuIFunc<ELF64LE>(S))
    return Plt_Explicit;

  switch (Type) {
  default:
    return Plt_No;
  case R_X86_64_32:
  case R_X86_64_32S:
  case R_X86_64_64:
  case R_X86_64_PC32:
    // This relocation is defined to have a value of (S + A - P).
    // The problems start when a non PIC program calls a function in a shared
    // library.
    // In an ideal world, we could just report an error saying the relocation
    // can overflow at runtime.
    // In the real world with glibc, crt1.o has a R_X86_64_PC32 pointing to
    // libc.so.
    //
    // The general idea on how to handle such cases is to create a PLT entry
    // and use that as the function value.
    //
    // For the static linking part, we just return true and everything else
    // will use the the PLT entry as the address.
    //
    // The remaining problem is making sure pointer equality still works. We
    // need the help of the dynamic linker for that. We let it know that we have
    // a direct reference to a so symbol by creating an undefined symbol with a
    // non zero st_value. Seeing that, the dynamic linker resolves the symbol to
    // the value of the symbol we created. This is true even for got entries, so
    // pointer equality is maintained. To avoid an infinite loop, the only entry
    // that points to the real function is a dedicated got entry used by the
    // plt. That is identified by special relocation types (R_X86_64_JUMP_SLOT,
    // R_386_JMP_SLOT, etc).
    if (auto *SS = dyn_cast<SharedSymbol<ELF64LE>>(&S)) {
      if (SS->Sym.getType() == STT_FUNC)
        return Plt_Implicit;
    }
    return Plt_No;
  case R_X86_64_PLT32:
    if (canBePreempted(&S, true))
      return Plt_Explicit;
    return Plt_No;
  }
}

bool X86_64TargetInfo::isRelRelative(uint32_t Type) const {
  switch (Type) {
  default:
    return false;
  case R_X86_64_DTPOFF32:
  case R_X86_64_DTPOFF64:
  case R_X86_64_PC8:
  case R_X86_64_PC16:
  case R_X86_64_PC32:
  case R_X86_64_PC64:
  case R_X86_64_PLT32:
    return true;
  }
}

bool X86_64TargetInfo::isSizeRel(uint32_t Type) const {
  return Type == R_X86_64_SIZE32 || Type == R_X86_64_SIZE64;
}

bool X86_64TargetInfo::canRelaxTls(unsigned Type, const SymbolBody *S) const {
  if (Config->Shared || (S && !S->IsTls))
    return false;
  return Type == R_X86_64_TLSGD || Type == R_X86_64_TLSLD ||
         Type == R_X86_64_DTPOFF32 ||
         (Type == R_X86_64_GOTTPOFF && !canBePreempted(S, true));
}

// "Ulrich Drepper, ELF Handling For Thread-Local Storage" (5.5
// x86-x64 linker optimizations, http://www.akkadia.org/drepper/tls.pdf) shows
// how LD can be optimized to LE:
//   leaq bar@tlsld(%rip), %rdi
//   callq __tls_get_addr@PLT
//   leaq bar@dtpoff(%rax), %rcx
// Is converted to:
//  .word 0x6666
//  .byte 0x66
//  mov %fs:0,%rax
//  leaq bar@tpoff(%rax), %rcx
void X86_64TargetInfo::relocateTlsLdToLe(uint8_t *Loc, uint8_t *BufEnd,
                                         uint64_t P, uint64_t SA) const {
  const uint8_t Inst[] = {
      0x66, 0x66,                                          //.word 0x6666
      0x66,                                                //.byte 0x66
      0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00 // mov %fs:0,%rax
  };
  memcpy(Loc - 3, Inst, sizeof(Inst));
}

// "Ulrich Drepper, ELF Handling For Thread-Local Storage" (5.5
// x86-x64 linker optimizations, http://www.akkadia.org/drepper/tls.pdf) shows
// how GD can be optimized to LE:
//  .byte 0x66
//  leaq x@tlsgd(%rip), %rdi
//  .word 0x6666
//  rex64
//  call __tls_get_addr@plt
// Is converted to:
//  mov %fs:0x0,%rax
//  lea x@tpoff,%rax
void X86_64TargetInfo::relocateTlsGdToLe(uint8_t *Loc, uint8_t *BufEnd,
                                         uint64_t P, uint64_t SA) const {
  const uint8_t Inst[] = {
      0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, // mov %fs:0x0,%rax
      0x48, 0x8d, 0x80, 0x00, 0x00, 0x00, 0x00              // lea x@tpoff,%rax
  };
  memcpy(Loc - 4, Inst, sizeof(Inst));
  relocateOne(Loc + 8, BufEnd, R_X86_64_TPOFF32, P, SA);
}

// "Ulrich Drepper, ELF Handling For Thread-Local Storage" (5.5
// x86-x64 linker optimizations, http://www.akkadia.org/drepper/tls.pdf) shows
// how GD can be optimized to IE:
//  .byte 0x66
//  leaq x@tlsgd(%rip), %rdi
//  .word 0x6666
//  rex64
//  call __tls_get_addr@plt
// Is converted to:
//  mov %fs:0x0,%rax
//  addq x@tpoff,%rax
void X86_64TargetInfo::relocateTlsGdToIe(uint8_t *Loc, uint8_t *BufEnd,
                                         uint64_t P, uint64_t SA) const {
  const uint8_t Inst[] = {
      0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, // mov %fs:0x0,%rax
      0x48, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00              // addq x@tpoff,%rax
  };
  memcpy(Loc - 4, Inst, sizeof(Inst));
  relocateOne(Loc + 8, BufEnd, R_X86_64_PC32, P + 12, SA);
}

// In some conditions, R_X86_64_GOTTPOFF relocation can be optimized to
// R_X86_64_TPOFF32 so that it does not use GOT.
// This function does that. Read "ELF Handling For Thread-Local Storage,
// 5.5 x86-x64 linker optimizations" (http://www.akkadia.org/drepper/tls.pdf)
// by Ulrich Drepper for details.
void X86_64TargetInfo::relocateTlsIeToLe(uint8_t *Loc, uint8_t *BufEnd,
                                         uint64_t P, uint64_t SA) const {
  // Ulrich's document section 6.5 says that @gottpoff(%rip) must be
  // used in MOVQ or ADDQ instructions only.
  // "MOVQ foo@GOTTPOFF(%RIP), %REG" is transformed to "MOVQ $foo, %REG".
  // "ADDQ foo@GOTTPOFF(%RIP), %REG" is transformed to "LEAQ foo(%REG), %REG"
  // (if the register is not RSP/R12) or "ADDQ $foo, %RSP".
  // Opcodes info can be found at http://ref.x86asm.net/coder64.html#x48.
  uint8_t *Prefix = Loc - 3;
  uint8_t *Inst = Loc - 2;
  uint8_t *RegSlot = Loc - 1;
  uint8_t Reg = Loc[-1] >> 3;
  bool IsMov = *Inst == 0x8b;
  bool RspAdd = !IsMov && Reg == 4;
  // r12 and rsp registers requires special handling.
  // Problem is that for other registers, for example leaq 0xXXXXXXXX(%r11),%r11
  // result out is 7 bytes: 4d 8d 9b XX XX XX XX,
  // but leaq 0xXXXXXXXX(%r12),%r12 is 8 bytes: 4d 8d a4 24 XX XX XX XX.
  // The same true for rsp. So we convert to addq for them, saving 1 byte that
  // we dont have.
  if (RspAdd)
    *Inst = 0x81;
  else
    *Inst = IsMov ? 0xc7 : 0x8d;
  if (*Prefix == 0x4c)
    *Prefix = (IsMov || RspAdd) ? 0x49 : 0x4d;
  *RegSlot = (IsMov || RspAdd) ? (0xc0 | Reg) : (0x80 | Reg | (Reg << 3));
  relocateOne(Loc, BufEnd, R_X86_64_TPOFF32, P, SA);
}

// This function applies a TLS relocation with an optimization as described
// in the Ulrich's document. As a result of rewriting instructions at the
// relocation target, relocations immediately follow the TLS relocation (which
// would be applied to rewritten instructions) may have to be skipped.
// This function returns a number of relocations that need to be skipped.
unsigned X86_64TargetInfo::relaxTls(uint8_t *Loc, uint8_t *BufEnd,
                                    uint32_t Type, uint64_t P, uint64_t SA,
                                    const SymbolBody *S) const {
  switch (Type) {
  case R_X86_64_DTPOFF32:
    relocateOne(Loc, BufEnd, R_X86_64_TPOFF32, P, SA);
    return 0;
  case R_X86_64_GOTTPOFF:
    relocateTlsIeToLe(Loc, BufEnd, P, SA);
    return 0;
  case R_X86_64_TLSGD: {
    if (canBePreempted(S, true))
      relocateTlsGdToIe(Loc, BufEnd, P, SA);
    else
      relocateTlsGdToLe(Loc, BufEnd, P, SA);
    // The next relocation should be against __tls_get_addr, so skip it
    return 1;
  }
  case R_X86_64_TLSLD:
    relocateTlsLdToLe(Loc, BufEnd, P, SA);
    // The next relocation should be against __tls_get_addr, so skip it
    return 1;
  }
  llvm_unreachable("Unknown TLS optimization");
}

void X86_64TargetInfo::relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type,
                                   uint64_t P, uint64_t SA, uint64_t ZA,
                                   uint8_t *PairedLoc) const {
  switch (Type) {
  case R_X86_64_32:
    checkUInt<32>(SA, Type);
    write32le(Loc, SA);
    break;
  case R_X86_64_32S:
    checkInt<32>(SA, Type);
    write32le(Loc, SA);
    break;
  case R_X86_64_64:
  case R_X86_64_DTPOFF64:
    write64le(Loc, SA);
    break;
  case R_X86_64_DTPOFF32:
    write32le(Loc, SA);
    break;
  case R_X86_64_GOTPCREL:
  case R_X86_64_PC32:
  case R_X86_64_PLT32:
  case R_X86_64_TLSGD:
  case R_X86_64_TLSLD:
    write32le(Loc, SA - P);
    break;
  case R_X86_64_SIZE32:
    write32le(Loc, ZA);
    break;
  case R_X86_64_SIZE64:
    write64le(Loc, ZA);
    break;
  case R_X86_64_TPOFF32: {
    uint64_t Val = SA - Out<ELF64LE>::TlsPhdr->p_memsz;
    checkInt<32>(Val, Type);
    write32le(Loc, Val);
    break;
  }
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

// Relocation masks following the #lo(value), #hi(value), #ha(value),
// #higher(value), #highera(value), #highest(value), and #highesta(value)
// macros defined in section 4.5.1. Relocation Types of the PPC-elf64abi
// document.
static uint16_t applyPPCLo(uint64_t V) { return V; }
static uint16_t applyPPCHi(uint64_t V) { return V >> 16; }
static uint16_t applyPPCHa(uint64_t V) { return (V + 0x8000) >> 16; }
static uint16_t applyPPCHigher(uint64_t V) { return V >> 32; }
static uint16_t applyPPCHighera(uint64_t V) { return (V + 0x8000) >> 32; }
static uint16_t applyPPCHighest(uint64_t V) { return V >> 48; }
static uint16_t applyPPCHighesta(uint64_t V) { return (V + 0x8000) >> 48; }

PPCTargetInfo::PPCTargetInfo() {}
bool PPCTargetInfo::isRelRelative(uint32_t Type) const { return false; }

void PPCTargetInfo::relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type,
                                uint64_t P, uint64_t SA, uint64_t ZA,
                                uint8_t *PairedLoc) const {
  switch (Type) {
  case R_PPC_ADDR16_HA:
    write16be(Loc, applyPPCHa(SA));
    break;
  case R_PPC_ADDR16_LO:
    write16be(Loc, applyPPCLo(SA));
    break;
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

PPC64TargetInfo::PPC64TargetInfo() {
  GotRel = R_PPC64_GLOB_DAT;
  RelativeRel = R_PPC64_RELATIVE;
  PltEntrySize = 32;

  // We need 64K pages (at least under glibc/Linux, the loader won't
  // set different permissions on a finer granularity than that).
  PageSize = 65536;

  // The PPC64 ELF ABI v1 spec, says:
  //
  //   It is normally desirable to put segments with different characteristics
  //   in separate 256 Mbyte portions of the address space, to give the
  //   operating system full paging flexibility in the 64-bit address space.
  //
  // And because the lowest non-zero 256M boundary is 0x10000000, PPC64 linkers
  // use 0x10000000 as the starting address.
  VAStart = 0x10000000;
}

uint64_t getPPC64TocBase() {
  // The TOC consists of sections .got, .toc, .tocbss, .plt in that
  // order. The TOC starts where the first of these sections starts.

  // FIXME: This obviously does not do the right thing when there is no .got
  // section, but there is a .toc or .tocbss section.
  uint64_t TocVA = Out<ELF64BE>::Got->getVA();
  if (!TocVA)
    TocVA = Out<ELF64BE>::Plt->getVA();

  // Per the ppc64-elf-linux ABI, The TOC base is TOC value plus 0x8000
  // thus permitting a full 64 Kbytes segment. Note that the glibc startup
  // code (crt1.o) assumes that you can get from the TOC base to the
  // start of the .toc section with only a single (signed) 16-bit relocation.
  return TocVA + 0x8000;
}

void PPC64TargetInfo::writePlt(uint8_t *Buf, uint64_t GotEntryAddr,
                               uint64_t PltEntryAddr, int32_t Index,
                               unsigned RelOff) const {
  uint64_t Off = GotEntryAddr - getPPC64TocBase();

  // FIXME: What we should do, in theory, is get the offset of the function
  // descriptor in the .opd section, and use that as the offset from %r2 (the
  // TOC-base pointer). Instead, we have the GOT-entry offset, and that will
  // be a pointer to the function descriptor in the .opd section. Using
  // this scheme is simpler, but requires an extra indirection per PLT dispatch.

  write32be(Buf,      0xf8410028);                   // std %r2, 40(%r1)
  write32be(Buf + 4,  0x3d620000 | applyPPCHa(Off)); // addis %r11, %r2, X@ha
  write32be(Buf + 8,  0xe98b0000 | applyPPCLo(Off)); // ld %r12, X@l(%r11)
  write32be(Buf + 12, 0xe96c0000);                   // ld %r11,0(%r12)
  write32be(Buf + 16, 0x7d6903a6);                   // mtctr %r11
  write32be(Buf + 20, 0xe84c0008);                   // ld %r2,8(%r12)
  write32be(Buf + 24, 0xe96c0010);                   // ld %r11,16(%r12)
  write32be(Buf + 28, 0x4e800420);                   // bctr
}

bool PPC64TargetInfo::needsGot(uint32_t Type, SymbolBody &S) const {
  if (needsPlt(Type, S))
    return true;

  switch (Type) {
  default: return false;
  case R_PPC64_GOT16:
  case R_PPC64_GOT16_DS:
  case R_PPC64_GOT16_HA:
  case R_PPC64_GOT16_HI:
  case R_PPC64_GOT16_LO:
  case R_PPC64_GOT16_LO_DS:
    return true;
  }
}

TargetInfo::PltNeed PPC64TargetInfo::needsPlt(uint32_t Type,
                                              const SymbolBody &S) const {
  // These are function calls that need to be redirected through a PLT stub.
  if (Type == R_PPC64_REL24 && canBePreempted(&S, false))
    return Plt_Explicit;
  return Plt_No;
}

bool PPC64TargetInfo::isRelRelative(uint32_t Type) const {
  switch (Type) {
  default:
    return true;
  case R_PPC64_ADDR64:
  case R_PPC64_TOC:
    return false;
  }
}

void PPC64TargetInfo::relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type,
                                  uint64_t P, uint64_t SA, uint64_t ZA,
                                  uint8_t *PairedLoc) const {
  uint64_t TB = getPPC64TocBase();

  // For a TOC-relative relocation, adjust the addend and proceed in terms of
  // the corresponding ADDR16 relocation type.
  switch (Type) {
  case R_PPC64_TOC16:       Type = R_PPC64_ADDR16;       SA -= TB; break;
  case R_PPC64_TOC16_DS:    Type = R_PPC64_ADDR16_DS;    SA -= TB; break;
  case R_PPC64_TOC16_HA:    Type = R_PPC64_ADDR16_HA;    SA -= TB; break;
  case R_PPC64_TOC16_HI:    Type = R_PPC64_ADDR16_HI;    SA -= TB; break;
  case R_PPC64_TOC16_LO:    Type = R_PPC64_ADDR16_LO;    SA -= TB; break;
  case R_PPC64_TOC16_LO_DS: Type = R_PPC64_ADDR16_LO_DS; SA -= TB; break;
  default: break;
  }

  switch (Type) {
  case R_PPC64_ADDR14: {
    checkAlignment<4>(SA, Type);
    // Preserve the AA/LK bits in the branch instruction
    uint8_t AALK = Loc[3];
    write16be(Loc + 2, (AALK & 3) | (SA & 0xfffc));
    break;
  }
  case R_PPC64_ADDR16:
    checkInt<16>(SA, Type);
    write16be(Loc, SA);
    break;
  case R_PPC64_ADDR16_DS:
    checkInt<16>(SA, Type);
    write16be(Loc, (read16be(Loc) & 3) | (SA & ~3));
    break;
  case R_PPC64_ADDR16_HA:
    write16be(Loc, applyPPCHa(SA));
    break;
  case R_PPC64_ADDR16_HI:
    write16be(Loc, applyPPCHi(SA));
    break;
  case R_PPC64_ADDR16_HIGHER:
    write16be(Loc, applyPPCHigher(SA));
    break;
  case R_PPC64_ADDR16_HIGHERA:
    write16be(Loc, applyPPCHighera(SA));
    break;
  case R_PPC64_ADDR16_HIGHEST:
    write16be(Loc, applyPPCHighest(SA));
    break;
  case R_PPC64_ADDR16_HIGHESTA:
    write16be(Loc, applyPPCHighesta(SA));
    break;
  case R_PPC64_ADDR16_LO:
    write16be(Loc, applyPPCLo(SA));
    break;
  case R_PPC64_ADDR16_LO_DS:
    write16be(Loc, (read16be(Loc) & 3) | (applyPPCLo(SA) & ~3));
    break;
  case R_PPC64_ADDR32:
    checkInt<32>(SA, Type);
    write32be(Loc, SA);
    break;
  case R_PPC64_ADDR64:
    write64be(Loc, SA);
    break;
  case R_PPC64_REL16_HA:
    write16be(Loc, applyPPCHa(SA - P));
    break;
  case R_PPC64_REL16_HI:
    write16be(Loc, applyPPCHi(SA - P));
    break;
  case R_PPC64_REL16_LO:
    write16be(Loc, applyPPCLo(SA - P));
    break;
  case R_PPC64_REL24: {
    // If we have an undefined weak symbol, we might get here with a symbol
    // address of zero. That could overflow, but the code must be unreachable,
    // so don't bother doing anything at all.
    if (!SA)
      break;

    uint64_t PltStart = Out<ELF64BE>::Plt->getVA();
    uint64_t PltEnd = PltStart + Out<ELF64BE>::Plt->getSize();
    bool InPlt = PltStart <= SA && SA < PltEnd;

    if (!InPlt && Out<ELF64BE>::Opd) {
      // If this is a local call, and we currently have the address of a
      // function-descriptor, get the underlying code address instead.
      uint64_t OpdStart = Out<ELF64BE>::Opd->getVA();
      uint64_t OpdEnd = OpdStart + Out<ELF64BE>::Opd->getSize();
      bool InOpd = OpdStart <= SA && SA < OpdEnd;

      if (InOpd)
        SA = read64be(&Out<ELF64BE>::OpdBuf[SA - OpdStart]);
    }

    uint32_t Mask = 0x03FFFFFC;
    checkInt<24>(SA - P, Type);
    write32be(Loc, (read32be(Loc) & ~Mask) | ((SA - P) & Mask));

    uint32_t Nop = 0x60000000;
    if (InPlt && Loc + 8 <= BufEnd && read32be(Loc + 4) == Nop)
      write32be(Loc + 4, 0xe8410028); // ld %r2, 40(%r1)
    break;
  }
  case R_PPC64_REL32:
    checkInt<32>(SA - P, Type);
    write32be(Loc, SA - P);
    break;
  case R_PPC64_REL64:
    write64be(Loc, SA - P);
    break;
  case R_PPC64_TOC:
    write64be(Loc, SA);
    break;
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

AArch64TargetInfo::AArch64TargetInfo() {
  CopyRel = R_AARCH64_COPY;
  RelativeRel = R_AARCH64_RELATIVE;
  IRelativeRel = R_AARCH64_IRELATIVE;
  GotRel = R_AARCH64_GLOB_DAT;
  PltRel = R_AARCH64_JUMP_SLOT;
  TlsGotRel = R_AARCH64_TLS_TPREL64;
  TlsModuleIndexRel = R_AARCH64_TLS_DTPMOD64;
  TlsOffsetRel = R_AARCH64_TLS_DTPREL64;
  UseLazyBinding = true;
  PltEntrySize = 16;
  PltZeroSize = 32;
}

bool AArch64TargetInfo::isRelRelative(uint32_t Type) const {
  return Type == R_AARCH64_PREL32;
}

bool AArch64TargetInfo::isTlsGlobalDynamicRel(unsigned Type) const {
  return Type == R_AARCH64_TLSDESC_ADR_PAGE21 ||
         Type == R_AARCH64_TLSDESC_LD64_LO12_NC ||
         Type == R_AARCH64_TLSDESC_ADD_LO12_NC ||
         Type == R_AARCH64_TLSDESC_CALL;
}

unsigned AArch64TargetInfo::getDynRel(unsigned Type) const {
  if (Type == R_AARCH64_ABS32 || Type == R_AARCH64_ABS64)
    return Type;
  StringRef S = getELFRelocationTypeName(EM_AARCH64, Type);
  error("Relocation " + S + " cannot be used when making a shared object; "
                            "recompile with -fPIC.");
  // Keep it going with a dummy value so that we can find more reloc errors.
  return R_AARCH64_ABS32;
}

void AArch64TargetInfo::writeGotPlt(uint8_t *Buf, uint64_t Plt) const {
  write64le(Buf, Out<ELF64LE>::Plt->getVA());
}

void AArch64TargetInfo::writePltZero(uint8_t *Buf) const {
  const uint8_t PltData[] = {
      0xf0, 0x7b, 0xbf, 0xa9, // stp	x16, x30, [sp,#-16]!
      0x10, 0x00, 0x00, 0x90, // adrp	x16, Page(&(.plt.got[2]))
      0x11, 0x02, 0x40, 0xf9, // ldr	x17, [x16, Offset(&(.plt.got[2]))]
      0x10, 0x02, 0x00, 0x91, // add	x16, x16, Offset(&(.plt.got[2]))
      0x20, 0x02, 0x1f, 0xd6, // br	x17
      0x1f, 0x20, 0x03, 0xd5, // nop
      0x1f, 0x20, 0x03, 0xd5, // nop
      0x1f, 0x20, 0x03, 0xd5  // nop
  };
  memcpy(Buf, PltData, sizeof(PltData));

  uint64_t Got = Out<ELF64LE>::GotPlt->getVA();
  uint64_t Plt = Out<ELF64LE>::Plt->getVA();
  relocateOne(Buf + 4, Buf + 8, R_AARCH64_ADR_PREL_PG_HI21, Plt + 4, Got + 16);
  relocateOne(Buf + 8, Buf + 12, R_AARCH64_LDST64_ABS_LO12_NC, Plt + 8,
              Got + 16);
  relocateOne(Buf + 12, Buf + 16, R_AARCH64_ADD_ABS_LO12_NC, Plt + 12,
              Got + 16);
}

void AArch64TargetInfo::writePlt(uint8_t *Buf, uint64_t GotEntryAddr,
                                 uint64_t PltEntryAddr, int32_t Index,
                                 unsigned RelOff) const {
  const uint8_t Inst[] = {
      0x10, 0x00, 0x00, 0x90, // adrp x16, Page(&(.plt.got[n]))
      0x11, 0x02, 0x40, 0xf9, // ldr  x17, [x16, Offset(&(.plt.got[n]))]
      0x10, 0x02, 0x00, 0x91, // add  x16, x16, Offset(&(.plt.got[n]))
      0x20, 0x02, 0x1f, 0xd6  // br   x17
  };
  memcpy(Buf, Inst, sizeof(Inst));

  relocateOne(Buf, Buf + 4, R_AARCH64_ADR_PREL_PG_HI21, PltEntryAddr,
              GotEntryAddr);
  relocateOne(Buf + 4, Buf + 8, R_AARCH64_LDST64_ABS_LO12_NC, PltEntryAddr + 4,
              GotEntryAddr);
  relocateOne(Buf + 8, Buf + 12, R_AARCH64_ADD_ABS_LO12_NC, PltEntryAddr + 8,
              GotEntryAddr);
}

unsigned AArch64TargetInfo::getTlsGotRel(unsigned Type) const {
  assert(Type == R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 ||
    Type == R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC);
    return Type;
}

bool AArch64TargetInfo::isTlsDynRel(unsigned Type, const SymbolBody &S) const {
  return Type == R_AARCH64_TLSDESC_ADR_PAGE21 ||
         Type == R_AARCH64_TLSDESC_LD64_LO12_NC ||
         Type == R_AARCH64_TLSDESC_ADD_LO12_NC ||
         Type == R_AARCH64_TLSDESC_CALL ||
         Type == R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 ||
         Type == R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC;
}

bool AArch64TargetInfo::needsCopyRelImpl(uint32_t Type) const {
  switch (Type) {
  default:
    return false;
  case R_AARCH64_ABS16:
  case R_AARCH64_ABS32:
  case R_AARCH64_ABS64:
  case R_AARCH64_ADD_ABS_LO12_NC:
  case R_AARCH64_ADR_PREL_LO21:
  case R_AARCH64_ADR_PREL_PG_HI21:
  case R_AARCH64_LDST8_ABS_LO12_NC:
  case R_AARCH64_LDST16_ABS_LO12_NC:
  case R_AARCH64_LDST32_ABS_LO12_NC:
  case R_AARCH64_LDST64_ABS_LO12_NC:
  case R_AARCH64_LDST128_ABS_LO12_NC:
    return true;
  }
}

bool AArch64TargetInfo::needsGot(uint32_t Type, SymbolBody &S) const {
  switch (Type) {
  case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC:
  case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21:
  case R_AARCH64_ADR_GOT_PAGE:
  case R_AARCH64_LD64_GOT_LO12_NC:
    return true;
  default:
    return needsPlt(Type, S);
  }
}

TargetInfo::PltNeed AArch64TargetInfo::needsPlt(uint32_t Type,
                                                const SymbolBody &S) const {
  if (isGnuIFunc<ELF64LE>(S))
    return Plt_Explicit;
  switch (Type) {
  default:
    return Plt_No;
  case R_AARCH64_CALL26:
  case R_AARCH64_CONDBR19:
  case R_AARCH64_JUMP26:
  case R_AARCH64_TSTBR14:
    if (canBePreempted(&S, true))
      return Plt_Explicit;
    return Plt_No;
  }
}

static void updateAArch64Addr(uint8_t *L, uint64_t Imm) {
  uint32_t ImmLo = (Imm & 0x3) << 29;
  uint32_t ImmHi = ((Imm & 0x1FFFFC) >> 2) << 5;
  uint64_t Mask = (0x3 << 29) | (0x7FFFF << 5);
  write32le(L, (read32le(L) & ~Mask) | ImmLo | ImmHi);
}

static inline void updateAArch64Add(uint8_t *L, uint64_t Imm) {
  or32le(L, (Imm & 0xFFF) << 10);
}

// Page(Expr) is the page address of the expression Expr, defined
// as (Expr & ~0xFFF). (This applies even if the machine page size
// supported by the platform has a different value.)
static uint64_t getAArch64Page(uint64_t Expr) {
  return Expr & (~static_cast<uint64_t>(0xFFF));
}

void AArch64TargetInfo::relocateOne(uint8_t *Loc, uint8_t *BufEnd,
                                    uint32_t Type, uint64_t P, uint64_t SA,
                                    uint64_t ZA, uint8_t *PairedLoc) const {
  switch (Type) {
  case R_AARCH64_ABS16:
    checkIntUInt<16>(SA, Type);
    write16le(Loc, SA);
    break;
  case R_AARCH64_ABS32:
    checkIntUInt<32>(SA, Type);
    write32le(Loc, SA);
    break;
  case R_AARCH64_ABS64:
    write64le(Loc, SA);
    break;
  case R_AARCH64_ADD_ABS_LO12_NC:
    // This relocation stores 12 bits and there's no instruction
    // to do it. Instead, we do a 32 bits store of the value
    // of r_addend bitwise-or'ed Loc. This assumes that the addend
    // bits in Loc are zero.
    or32le(Loc, (SA & 0xFFF) << 10);
    break;
  case R_AARCH64_ADR_GOT_PAGE: {
    uint64_t X = getAArch64Page(SA) - getAArch64Page(P);
    checkInt<33>(X, Type);
    updateAArch64Addr(Loc, (X >> 12) & 0x1FFFFF); // X[32:12]
    break;
  }
  case R_AARCH64_ADR_PREL_LO21: {
    uint64_t X = SA - P;
    checkInt<21>(X, Type);
    updateAArch64Addr(Loc, X & 0x1FFFFF);
    break;
  }
  case R_AARCH64_ADR_PREL_PG_HI21:
  case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21: {
    uint64_t X = getAArch64Page(SA) - getAArch64Page(P);
    checkInt<33>(X, Type);
    updateAArch64Addr(Loc, (X >> 12) & 0x1FFFFF); // X[32:12]
    break;
  }
  case R_AARCH64_CALL26:
  case R_AARCH64_JUMP26: {
    uint64_t X = SA - P;
    checkInt<28>(X, Type);
    or32le(Loc, (X & 0x0FFFFFFC) >> 2);
    break;
  }
  case R_AARCH64_CONDBR19: {
    uint64_t X = SA - P;
    checkInt<21>(X, Type);
    or32le(Loc, (X & 0x1FFFFC) << 3);
    break;
  }
  case R_AARCH64_LD64_GOT_LO12_NC:
  case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC:
    checkAlignment<8>(SA, Type);
    or32le(Loc, (SA & 0xFF8) << 7);
    break;
  case R_AARCH64_LDST128_ABS_LO12_NC:
    or32le(Loc, (SA & 0x0FF8) << 6);
    break;
  case R_AARCH64_LDST16_ABS_LO12_NC:
    or32le(Loc, (SA & 0x0FFC) << 9);
    break;
  case R_AARCH64_LDST8_ABS_LO12_NC:
    or32le(Loc, (SA & 0xFFF) << 10);
    break;
  case R_AARCH64_LDST32_ABS_LO12_NC:
    or32le(Loc, (SA & 0xFFC) << 8);
    break;
  case R_AARCH64_LDST64_ABS_LO12_NC:
    or32le(Loc, (SA & 0xFF8) << 7);
    break;
  case R_AARCH64_PREL16:
    checkIntUInt<16>(SA - P, Type);
    write16le(Loc, SA - P);
    break;
  case R_AARCH64_PREL32:
    checkIntUInt<32>(SA - P, Type);
    write32le(Loc, SA - P);
    break;
  case R_AARCH64_PREL64:
    write64le(Loc, SA - P);
    break;
  case R_AARCH64_TSTBR14: {
    uint64_t X = SA - P;
    checkInt<16>(X, Type);
    or32le(Loc, (X & 0xFFFC) << 3);
    break;
  }
  case R_AARCH64_TLSLE_ADD_TPREL_HI12: {
    uint64_t V = llvm::alignTo(TcbSize, Out<ELF64LE>::TlsPhdr->p_align) + SA;
    checkInt<24>(V, Type);
    updateAArch64Add(Loc, (V & 0xFFF000) >> 12);
    break;
  }
  case R_AARCH64_TLSLE_ADD_TPREL_LO12_NC: {
    uint64_t V = llvm::alignTo(TcbSize, Out<ELF64LE>::TlsPhdr->p_align) + SA;
    updateAArch64Add(Loc, V & 0xFFF);
    break;
  }
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

bool AArch64TargetInfo::canRelaxTls(unsigned Type, const SymbolBody *S) const {
  if (Config->Shared || (S && !S->IsTls))
    return false;

  // Global-Dynamic relocs can be relaxed to Initial-Exec if the target is
  // an executable.  And if the target is local it can also be fully relaxed to
  // Local-Exec.
  if (isTlsGlobalDynamicRel(Type))
    return !canBePreempted(S, true);

  // Initial-Exec relocs can be relaxed to Local-Exec if the target is a local
  // symbol.
  if (Type == R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 ||
      Type == R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC)
    return !canBePreempted(S, true);

  return false;
}

unsigned AArch64TargetInfo::relaxTls(uint8_t *Loc, uint8_t *BufEnd,
                                     uint32_t Type, uint64_t P, uint64_t SA,
                                     const SymbolBody *S) const {
  switch (Type) {
  case R_AARCH64_TLSDESC_ADR_PAGE21:
  case R_AARCH64_TLSDESC_LD64_LO12_NC:
  case R_AARCH64_TLSDESC_ADD_LO12_NC:
  case R_AARCH64_TLSDESC_CALL: {
    if (canBePreempted(S, true))
      fatal("Unsupported TLS optimization");
    uint64_t X = S ? S->getVA<ELF64LE>() : SA;
    relocateTlsGdToLe(Type, Loc, BufEnd, P, X);
    return 0;
  }
  case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21:
  case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC:
    relocateTlsIeToLe(Type, Loc, BufEnd, P, S->getVA<ELF64LE>());
    return 0;
  }
  llvm_unreachable("Unknown TLS optimization");
}

// Global-Dynamic relocations can be relaxed to Local-Exec if both binary is
// an executable and target is final (can notbe preempted).
void AArch64TargetInfo::relocateTlsGdToLe(unsigned Type, uint8_t *Loc,
                                          uint8_t *BufEnd, uint64_t P,
                                          uint64_t SA) const {
  // TLSDESC Global-Dynamic relocation are in the form:
  //   adrp    x0, :tlsdesc:v             [R_AARCH64_TLSDESC_ADR_PAGE21]
  //   ldr     x1, [x0, #:tlsdesc_lo12:v  [R_AARCH64_TLSDESC_LD64_LO12_NC]
  //   add     x0, x0, :tlsdesc_los:v     [_AARCH64_TLSDESC_ADD_LO12_NC]
  //   .tlsdesccall                       [R_AARCH64_TLSDESC_CALL]
  // And it can optimized to:
  //   movz    x0, #0x0, lsl #16
  //   movk    x0, #0x10
  //   nop
  //   nop

  uint64_t TPOff = llvm::alignTo(TcbSize, Out<ELF64LE>::TlsPhdr->p_align);
  uint64_t X = SA + TPOff;
  checkUInt<32>(X, Type);

  uint32_t NewInst;
  switch (Type) {
  case R_AARCH64_TLSDESC_ADD_LO12_NC:
  case R_AARCH64_TLSDESC_CALL:
    // nop
    NewInst = 0xd503201f;
    break;
  case R_AARCH64_TLSDESC_ADR_PAGE21:
    // movz
    NewInst = 0xd2a00000 | (((X >> 16) & 0xffff) << 5);
    break;
  case R_AARCH64_TLSDESC_LD64_LO12_NC:
    // movk
    NewInst = 0xf2800000 | ((X & 0xffff) << 5);
    break;
  default:
    llvm_unreachable("Unsupported Relocation for TLS GD to LE relax");
  }
  write32le(Loc, NewInst);
}

// Initial-Exec relocations can be relaxed to Local-Exec if symbol is final
// (can not be preempted).
void AArch64TargetInfo::relocateTlsIeToLe(unsigned Type, uint8_t *Loc,
                                          uint8_t *BufEnd, uint64_t P,
                                          uint64_t SA) const {
  uint64_t TPOff = llvm::alignTo(TcbSize, Out<ELF64LE>::TlsPhdr->p_align);
  uint64_t X = SA + TPOff;
  checkUInt<32>(X, Type);

  uint32_t Inst = read32le (Loc);
  uint32_t NewInst;
  if (Type == R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21) {
    // Generate movz.
    unsigned RegNo = (Inst & 0x1f);
    NewInst = (0xd2a00000 | RegNo) | (((X >> 16) & 0xffff) << 5);
  } else if (Type == R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC) {
    // Generate movk
    unsigned RegNo = (Inst & 0x1f);
    NewInst = (0xf2800000 | RegNo) | ((X & 0xffff) << 5);
  } else {
    llvm_unreachable("Invalid Relocation for TLS IE to LE Relax");
  }
  write32le(Loc, NewInst);
}


// Implementing relocations for AMDGPU is low priority since most
// programs don't use relocations now. Thus, this function is not
// actually called (relocateOne is called for each relocation).
// That's why the AMDGPU port works without implementing this function.
void AMDGPUTargetInfo::relocateOne(uint8_t *Loc, uint8_t *BufEnd, uint32_t Type,
                                   uint64_t P, uint64_t SA, uint64_t ZA,
                                   uint8_t *PairedLoc) const {
  llvm_unreachable("not implemented");
}

template <class ELFT> MipsTargetInfo<ELFT>::MipsTargetInfo() {
  GotHeaderEntriesNum = 2;
  GotPltHeaderEntriesNum = 2;
  PageSize = 65536;
  PltEntrySize = 16;
  PltZeroSize = 32;
  UseLazyBinding = true;
  CopyRel = R_MIPS_COPY;
  PltRel = R_MIPS_JUMP_SLOT;
  RelativeRel = R_MIPS_REL32;
}

template <class ELFT>
unsigned MipsTargetInfo<ELFT>::getDynRel(unsigned Type) const {
  if (Type == R_MIPS_32 || Type == R_MIPS_64)
    return R_MIPS_REL32;
  StringRef S = getELFRelocationTypeName(EM_MIPS, Type);
  error("Relocation " + S + " cannot be used when making a shared object; "
                            "recompile with -fPIC.");
  // Keep it going with a dummy value so that we can find more reloc errors.
  return R_MIPS_32;
}

template <class ELFT>
void MipsTargetInfo<ELFT>::writeGotHeader(uint8_t *Buf) const {
  typedef typename ELFFile<ELFT>::Elf_Off Elf_Off;
  typedef typename ELFFile<ELFT>::uintX_t uintX_t;

  // Set the MSB of the second GOT slot. This is not required by any
  // MIPS ABI documentation, though.
  //
  // There is a comment in glibc saying that "The MSB of got[1] of a
  // gnu object is set to identify gnu objects," and in GNU gold it
  // says "the second entry will be used by some runtime loaders".
  // But how this field is being used is unclear.
  //
  // We are not really willing to mimic other linkers behaviors
  // without understanding why they do that, but because all files
  // generated by GNU tools have this special GOT value, and because
  // we've been doing this for years, it is probably a safe bet to
  // keep doing this for now. We really need to revisit this to see
  // if we had to do this.
  auto *P = reinterpret_cast<Elf_Off *>(Buf);
  P[1] = uintX_t(1) << (ELFT::Is64Bits ? 63 : 31);
}

template <class ELFT>
void MipsTargetInfo<ELFT>::writeGotPlt(uint8_t *Buf, uint64_t Plt) const {
  write32<ELFT::TargetEndianness>(Buf, Out<ELFT>::Plt->getVA());
}

static uint16_t mipsHigh(uint64_t V) { return (V + 0x8000) >> 16; }

template <endianness E, uint8_t BSIZE, uint8_t SHIFT>
static void applyMipsPcReloc(uint8_t *Loc, uint32_t Type, uint64_t P,
                             uint64_t S) {
  uint32_t Mask = 0xffffffff >> (32 - BSIZE);
  uint32_t Instr = read32<E>(Loc);
  int64_t A = SignExtend64<BSIZE + SHIFT>((Instr & Mask) << SHIFT);
  if (SHIFT > 0)
    checkAlignment<(1 << SHIFT)>(S + A, Type);
  int64_t V = S + A - P;
  checkInt<BSIZE + SHIFT>(V, Type);
  write32<E>(Loc, (Instr & ~Mask) | ((V >> SHIFT) & Mask));
}

template <endianness E>
static void applyMipsHi16Reloc(uint8_t *Loc, uint64_t S, int64_t A) {
  uint32_t Instr = read32<E>(Loc);
  write32<E>(Loc, (Instr & 0xffff0000) | mipsHigh(S + A));
}

template <class ELFT>
void MipsTargetInfo<ELFT>::writePltZero(uint8_t *Buf) const {
  const endianness E = ELFT::TargetEndianness;
  write32<E>(Buf, 0x3c1c0000);      // lui   $28, %hi(&GOTPLT[0])
  write32<E>(Buf + 4, 0x8f990000);  // lw    $25, %lo(&GOTPLT[0])($28)
  write32<E>(Buf + 8, 0x279c0000);  // addiu $28, $28, %lo(&GOTPLT[0])
  write32<E>(Buf + 12, 0x031cc023); // subu  $24, $24, $28
  write32<E>(Buf + 16, 0x03e07825); // move  $15, $31
  write32<E>(Buf + 20, 0x0018c082); // srl   $24, $24, 2
  write32<E>(Buf + 24, 0x0320f809); // jalr  $25
  write32<E>(Buf + 28, 0x2718fffe); // subu  $24, $24, 2
  uint64_t Got = Out<ELFT>::GotPlt->getVA();
  uint64_t Plt = Out<ELFT>::Plt->getVA();
  applyMipsHi16Reloc<E>(Buf, Got, 0);
  relocateOne(Buf + 4, Buf + 8, R_MIPS_LO16, Plt + 4, Got);
  relocateOne(Buf + 8, Buf + 12, R_MIPS_LO16, Plt + 8, Got);
}

template <class ELFT>
void MipsTargetInfo<ELFT>::writePlt(uint8_t *Buf, uint64_t GotEntryAddr,
                                    uint64_t PltEntryAddr, int32_t Index,
                                    unsigned RelOff) const {
  const endianness E = ELFT::TargetEndianness;
  write32<E>(Buf, 0x3c0f0000);      // lui   $15, %hi(.got.plt entry)
  write32<E>(Buf + 4, 0x8df90000);  // l[wd] $25, %lo(.got.plt entry)($15)
  write32<E>(Buf + 8, 0x03200008);  // jr    $25
  write32<E>(Buf + 12, 0x25f80000); // addiu $24, $15, %lo(.got.plt entry)
  applyMipsHi16Reloc<E>(Buf, GotEntryAddr, 0);
  relocateOne(Buf + 4, Buf + 8, R_MIPS_LO16, PltEntryAddr + 4, GotEntryAddr);
  relocateOne(Buf + 12, Buf + 16, R_MIPS_LO16, PltEntryAddr + 8, GotEntryAddr);
}

template <class ELFT>
bool MipsTargetInfo<ELFT>::needsCopyRelImpl(uint32_t Type) const {
  return Type == R_MIPS_HI16 || Type == R_MIPS_LO16 || isRelRelative(Type);
}

template <class ELFT>
bool MipsTargetInfo<ELFT>::needsGot(uint32_t Type, SymbolBody &S) const {
  return needsPlt(Type, S) || Type == R_MIPS_GOT16 || Type == R_MIPS_CALL16;
}

template <class ELFT>
TargetInfo::PltNeed MipsTargetInfo<ELFT>::needsPlt(uint32_t Type,
                                                   const SymbolBody &S) const {
  if (needsCopyRel<ELFT>(Type, S))
    return Plt_No;
  if (Type == R_MIPS_26 && canBePreempted(&S, false))
    return Plt_Explicit;
  if (Type == R_MIPS_HI16 || Type == R_MIPS_LO16 || isRelRelative(Type))
    if (S.isShared())
      return Plt_Explicit;
  return Plt_No;
}

template <class ELFT>
void MipsTargetInfo<ELFT>::relocateOne(uint8_t *Loc, uint8_t *BufEnd,
                                       uint32_t Type, uint64_t P, uint64_t S,
                                       uint64_t ZA, uint8_t *PairedLoc) const {
  const endianness E = ELFT::TargetEndianness;
  switch (Type) {
  case R_MIPS_32:
    add32<E>(Loc, S);
    break;
  case R_MIPS_26: {
    uint32_t Instr = read32<E>(Loc);
    // FIXME (simon): If the relocation target symbol is not a PLT entry
    // we should use another expression for calculation:
    // ((A << 2) | (P & 0xf0000000)) >> 2
    S += SignExtend64<28>((Instr & 0x3ffffff) << 2);
    write32<E>(Loc, (Instr & ~0x3ffffff) | (S >> 2));
    break;
  }
  case R_MIPS_CALL16:
  case R_MIPS_GOT16: {
    int64_t V = S - getMipsGpAddr<ELFT>();
    if (Type == R_MIPS_GOT16)
      checkInt<16>(V, Type);
    write32<E>(Loc, (read32<E>(Loc) & 0xffff0000) | (V & 0xffff));
    break;
  }
  case R_MIPS_GPREL16: {
    uint32_t Instr = read32<E>(Loc);
    int64_t V = S + SignExtend64<16>(Instr & 0xffff) - getMipsGpAddr<ELFT>();
    checkInt<16>(V, Type);
    write32<E>(Loc, (Instr & 0xffff0000) | (V & 0xffff));
    break;
  }
  case R_MIPS_GPREL32:
    write32<E>(Loc, S + int32_t(read32<E>(Loc)) - getMipsGpAddr<ELFT>());
    break;
  case R_MIPS_HI16: {
    uint32_t Instr = read32<E>(Loc);
    if (PairedLoc) {
      uint64_t AHL = ((Instr & 0xffff) << 16) +
                     SignExtend64<16>(read32<E>(PairedLoc) & 0xffff);
      applyMipsHi16Reloc<E>(Loc, S, AHL);
    } else {
      warning("Can't find matching R_MIPS_LO16 relocation for R_MIPS_HI16");
      applyMipsHi16Reloc<E>(Loc, S, 0);
    }
    break;
  }
  case R_MIPS_JALR:
    // Ignore this optimization relocation for now
    break;
  case R_MIPS_LO16: {
    uint32_t Instr = read32<E>(Loc);
    int64_t AHL = SignExtend64<16>(Instr & 0xffff);
    write32<E>(Loc, (Instr & 0xffff0000) | ((S + AHL) & 0xffff));
    break;
  }
  case R_MIPS_PC16:
    applyMipsPcReloc<E, 16, 2>(Loc, Type, P, S);
    break;
  case R_MIPS_PC19_S2:
    applyMipsPcReloc<E, 19, 2>(Loc, Type, P, S);
    break;
  case R_MIPS_PC21_S2:
    applyMipsPcReloc<E, 21, 2>(Loc, Type, P, S);
    break;
  case R_MIPS_PC26_S2:
    applyMipsPcReloc<E, 26, 2>(Loc, Type, P, S);
    break;
  case R_MIPS_PC32:
    applyMipsPcReloc<E, 32, 0>(Loc, Type, P, S);
    break;
  case R_MIPS_PCHI16: {
    uint32_t Instr = read32<E>(Loc);
    if (PairedLoc) {
      uint64_t AHL = ((Instr & 0xffff) << 16) +
                     SignExtend64<16>(read32<E>(PairedLoc) & 0xffff);
      write32<E>(Loc, (Instr & 0xffff0000) | mipsHigh(S + AHL - P));
    } else {
      warning("Can't find matching R_MIPS_PCLO16 relocation for R_MIPS_PCHI16");
      write32<E>(Loc, (Instr & 0xffff0000) | mipsHigh(S - P));
    }
    break;
  }
  case R_MIPS_PCLO16: {
    uint32_t Instr = read32<E>(Loc);
    int64_t AHL = SignExtend64<16>(Instr & 0xffff);
    write32<E>(Loc, (Instr & 0xffff0000) | ((S + AHL - P) & 0xffff));
    break;
  }
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

template <class ELFT>
bool MipsTargetInfo<ELFT>::isHintRel(uint32_t Type) const {
  return Type == R_MIPS_JALR;
}

template <class ELFT>
bool MipsTargetInfo<ELFT>::isRelRelative(uint32_t Type) const {
  switch (Type) {
  default:
    return false;
  case R_MIPS_PC16:
  case R_MIPS_PC19_S2:
  case R_MIPS_PC21_S2:
  case R_MIPS_PC26_S2:
  case R_MIPS_PC32:
  case R_MIPS_PCHI16:
  case R_MIPS_PCLO16:
    return true;
  }
}

// _gp is a MIPS-specific ABI-defined symbol which points to
// a location that is relative to GOT. This function returns
// the value for the symbol.
template <class ELFT> typename ELFFile<ELFT>::uintX_t getMipsGpAddr() {
  unsigned GPOffset = 0x7ff0;
  if (uint64_t V = Out<ELFT>::Got->getVA())
    return V + GPOffset;
  return 0;
}

template bool isGnuIFunc<ELF32LE>(const SymbolBody &S);
template bool isGnuIFunc<ELF32BE>(const SymbolBody &S);
template bool isGnuIFunc<ELF64LE>(const SymbolBody &S);
template bool isGnuIFunc<ELF64BE>(const SymbolBody &S);

template uint32_t getMipsGpAddr<ELF32LE>();
template uint32_t getMipsGpAddr<ELF32BE>();
template uint64_t getMipsGpAddr<ELF64LE>();
template uint64_t getMipsGpAddr<ELF64BE>();

template bool TargetInfo::needsCopyRel<ELF32LE>(uint32_t,
                                                const SymbolBody &) const;
template bool TargetInfo::needsCopyRel<ELF32BE>(uint32_t,
                                                const SymbolBody &) const;
template bool TargetInfo::needsCopyRel<ELF64LE>(uint32_t,
                                                const SymbolBody &) const;
template bool TargetInfo::needsCopyRel<ELF64BE>(uint32_t,
                                                const SymbolBody &) const;
}
}
