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
// in this file.
//
// Some functions defined in this file has "relaxTls" as part of their names.
// They do peephole optimization for TLS variables by rewriting instructions.
// They are not part of the ABI but optional optimization, so you can skip
// them if you are not interested in how TLS variables are optimized.
// See the following paper for the details.
//
//   Ulrich Drepper, ELF Handling For Thread-Local Storage
//   http://www.akkadia.org/drepper/tls.pdf
//
//===----------------------------------------------------------------------===//

#include "Target.h"
#include "Error.h"
#include "InputFiles.h"
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
namespace elf {

TargetInfo *Target;

static void or32le(uint8_t *P, int32_t V) { write32le(P, read32le(P) | V); }

StringRef getRelName(uint32_t Type) {
  return getELFRelocationTypeName(Config->EMachine, Type);
}

template <unsigned N> static void checkInt(int64_t V, uint32_t Type) {
  if (isInt<N>(V))
    return;
  error("relocation " + getRelName(Type) + " out of range");
}

template <unsigned N> static void checkUInt(uint64_t V, uint32_t Type) {
  if (isUInt<N>(V))
    return;
  error("relocation " + getRelName(Type) + " out of range");
}

template <unsigned N> static void checkIntUInt(uint64_t V, uint32_t Type) {
  if (isInt<N>(V) || isUInt<N>(V))
    return;
  error("relocation " + getRelName(Type) + " out of range");
}

template <unsigned N> static void checkAlignment(uint64_t V, uint32_t Type) {
  if ((V & (N - 1)) == 0)
    return;
  error("improper alignment for relocation " + getRelName(Type));
}

static void errorDynRel(uint32_t Type) {
  error("relocation " + getRelName(Type) +
        " cannot be used when making a shared object; recompile with -fPIC.");
}

namespace {
class X86TargetInfo final : public TargetInfo {
public:
  X86TargetInfo();
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S) const override;
  uint64_t getImplicitAddend(const uint8_t *Buf, uint32_t Type) const override;
  void writeGotPltHeader(uint8_t *Buf) const override;
  uint32_t getDynRel(uint32_t Type) const override;
  bool isTlsLocalDynamicRel(uint32_t Type) const override;
  bool isTlsGlobalDynamicRel(uint32_t Type) const override;
  bool isTlsInitialExecRel(uint32_t Type) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;

  RelExpr adjustRelaxExpr(uint32_t Type, const uint8_t *Data,
                          RelExpr Expr) const override;
  void relaxTlsGdToIe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  void relaxTlsGdToLe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  void relaxTlsIeToLe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  void relaxTlsLdToLe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
};

class X86_64TargetInfo final : public TargetInfo {
public:
  X86_64TargetInfo();
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S) const override;
  uint32_t getDynRel(uint32_t Type) const override;
  bool isTlsLocalDynamicRel(uint32_t Type) const override;
  bool isTlsGlobalDynamicRel(uint32_t Type) const override;
  bool isTlsInitialExecRel(uint32_t Type) const override;
  void writeGotPltHeader(uint8_t *Buf) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;

  RelExpr adjustRelaxExpr(uint32_t Type, const uint8_t *Data,
                          RelExpr Expr) const override;
  void relaxGot(uint8_t *Loc, uint64_t Val) const override;
  void relaxTlsGdToIe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  void relaxTlsGdToLe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  void relaxTlsIeToLe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  void relaxTlsLdToLe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;

private:
  void relaxGotNoPic(uint8_t *Loc, uint64_t Val, uint8_t Op,
                     uint8_t ModRm) const;
};

class PPCTargetInfo final : public TargetInfo {
public:
  PPCTargetInfo();
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S) const override;
};

class PPC64TargetInfo final : public TargetInfo {
public:
  PPC64TargetInfo();
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
};

class AArch64TargetInfo final : public TargetInfo {
public:
  AArch64TargetInfo();
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S) const override;
  uint32_t getDynRel(uint32_t Type) const override;
  bool isTlsInitialExecRel(uint32_t Type) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  bool usesOnlyLowPageBits(uint32_t Type) const override;
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  RelExpr adjustRelaxExpr(uint32_t Type, const uint8_t *Data,
                          RelExpr Expr) const override;
  void relaxTlsGdToLe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  void relaxTlsGdToIe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  void relaxTlsIeToLe(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
};

class AMDGPUTargetInfo final : public TargetInfo {
public:
  AMDGPUTargetInfo() {}
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S) const override;
};

class ARMTargetInfo final : public TargetInfo {
public:
  ARMTargetInfo();
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S) const override;
  uint32_t getDynRel(uint32_t Type) const override;
  uint64_t getImplicitAddend(const uint8_t *Buf, uint32_t Type) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
};

template <class ELFT> class MipsTargetInfo final : public TargetInfo {
public:
  MipsTargetInfo();
  RelExpr getRelExpr(uint32_t Type, const SymbolBody &S) const override;
  uint64_t getImplicitAddend(const uint8_t *Buf, uint32_t Type) const override;
  uint32_t getDynRel(uint32_t Type) const override;
  void writeGotPlt(uint8_t *Buf, uint64_t Plt) const override;
  void writePltZero(uint8_t *Buf) const override;
  void writePlt(uint8_t *Buf, uint64_t GotEntryAddr, uint64_t PltEntryAddr,
                int32_t Index, unsigned RelOff) const override;
  void writeThunk(uint8_t *Buf, uint64_t S) const override;
  bool needsThunk(uint32_t Type, const InputFile &File,
                  const SymbolBody &S) const override;
  void relocateOne(uint8_t *Loc, uint32_t Type, uint64_t Val) const override;
  bool usesOnlyLowPageBits(uint32_t Type) const override;
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
  case EM_ARM:
    return new ARMTargetInfo();
  case EM_MIPS:
    switch (Config->EKind) {
    case ELF32LEKind:
      return new MipsTargetInfo<ELF32LE>();
    case ELF32BEKind:
      return new MipsTargetInfo<ELF32BE>();
    case ELF64LEKind:
      return new MipsTargetInfo<ELF64LE>();
    case ELF64BEKind:
      return new MipsTargetInfo<ELF64BE>();
    default:
      fatal("unsupported MIPS target");
    }
  case EM_PPC:
    return new PPCTargetInfo();
  case EM_PPC64:
    return new PPC64TargetInfo();
  case EM_X86_64:
    return new X86_64TargetInfo();
  }
  fatal("unknown target machine");
}

TargetInfo::~TargetInfo() {}

uint64_t TargetInfo::getImplicitAddend(const uint8_t *Buf,
                                       uint32_t Type) const {
  return 0;
}

uint64_t TargetInfo::getVAStart() const { return Config->Pic ? 0 : VAStart; }

bool TargetInfo::usesOnlyLowPageBits(uint32_t Type) const { return false; }

bool TargetInfo::needsThunk(uint32_t Type, const InputFile &File,
                            const SymbolBody &S) const {
  return false;
}

bool TargetInfo::isTlsInitialExecRel(uint32_t Type) const { return false; }

bool TargetInfo::isTlsLocalDynamicRel(uint32_t Type) const { return false; }

bool TargetInfo::isTlsGlobalDynamicRel(uint32_t Type) const {
  return false;
}

RelExpr TargetInfo::adjustRelaxExpr(uint32_t Type, const uint8_t *Data,
                                    RelExpr Expr) const {
  return Expr;
}

void TargetInfo::relaxGot(uint8_t *Loc, uint64_t Val) const {
  llvm_unreachable("Should not have claimed to be relaxable");
}

void TargetInfo::relaxTlsGdToLe(uint8_t *Loc, uint32_t Type,
                                uint64_t Val) const {
  llvm_unreachable("Should not have claimed to be relaxable");
}

void TargetInfo::relaxTlsGdToIe(uint8_t *Loc, uint32_t Type,
                                uint64_t Val) const {
  llvm_unreachable("Should not have claimed to be relaxable");
}

void TargetInfo::relaxTlsIeToLe(uint8_t *Loc, uint32_t Type,
                                uint64_t Val) const {
  llvm_unreachable("Should not have claimed to be relaxable");
}

void TargetInfo::relaxTlsLdToLe(uint8_t *Loc, uint32_t Type,
                                uint64_t Val) const {
  llvm_unreachable("Should not have claimed to be relaxable");
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
  PltEntrySize = 16;
  PltZeroSize = 16;
  TlsGdRelaxSkip = 2;
}

RelExpr X86TargetInfo::getRelExpr(uint32_t Type, const SymbolBody &S) const {
  switch (Type) {
  default:
    return R_ABS;
  case R_386_TLS_GD:
    return R_TLSGD;
  case R_386_TLS_LDM:
    return R_TLSLD;
  case R_386_PLT32:
    return R_PLT_PC;
  case R_386_PC32:
    return R_PC;
  case R_386_GOTPC:
    return R_GOTONLY_PC;
  case R_386_TLS_IE:
    return R_GOT;
  case R_386_GOT32:
  case R_386_TLS_GOTIE:
    return R_GOT_FROM_END;
  case R_386_GOTOFF:
    return R_GOTREL;
  case R_386_TLS_LE:
    return R_TLS;
  case R_386_TLS_LE_32:
    return R_NEG_TLS;
  }
}

RelExpr X86TargetInfo::adjustRelaxExpr(uint32_t Type, const uint8_t *Data,
                                       RelExpr Expr) const {
  switch (Expr) {
  default:
    return Expr;
  case R_RELAX_TLS_GD_TO_IE:
    return R_RELAX_TLS_GD_TO_IE_END;
  case R_RELAX_TLS_GD_TO_LE:
    return R_RELAX_TLS_GD_TO_LE_NEG;
  }
}

void X86TargetInfo::writeGotPltHeader(uint8_t *Buf) const {
  write32le(Buf, Out<ELF32LE>::Dynamic->getVA());
}

void X86TargetInfo::writeGotPlt(uint8_t *Buf, uint64_t Plt) const {
  // Entries in .got.plt initially points back to the corresponding
  // PLT entries with a fixed offset to skip the first instruction.
  write32le(Buf, Plt + 6);
}

uint32_t X86TargetInfo::getDynRel(uint32_t Type) const {
  if (Type == R_386_TLS_LE)
    return R_386_TLS_TPOFF;
  if (Type == R_386_TLS_LE_32)
    return R_386_TLS_TPOFF32;
  return Type;
}

bool X86TargetInfo::isTlsGlobalDynamicRel(uint32_t Type) const {
  return Type == R_386_TLS_GD;
}

bool X86TargetInfo::isTlsLocalDynamicRel(uint32_t Type) const {
  return Type == R_386_TLS_LDO_32 || Type == R_386_TLS_LDM;
}

bool X86TargetInfo::isTlsInitialExecRel(uint32_t Type) const {
  return Type == R_386_TLS_IE || Type == R_386_TLS_GOTIE;
}

void X86TargetInfo::writePltZero(uint8_t *Buf) const {
  // Executable files and shared object files have
  // separate procedure linkage tables.
  if (Config->Pic) {
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
  Buf[1] = Config->Pic ? 0xa3 : 0x25;
  uint32_t Got = Out<ELF32LE>::GotPlt->getVA();
  write32le(Buf + 2, Config->Shared ? GotEntryAddr - Got : GotEntryAddr);
  write32le(Buf + 7, RelOff);
  write32le(Buf + 12, -Index * PltEntrySize - PltZeroSize - 16);
}

uint64_t X86TargetInfo::getImplicitAddend(const uint8_t *Buf,
                                          uint32_t Type) const {
  switch (Type) {
  default:
    return 0;
  case R_386_32:
  case R_386_GOT32:
  case R_386_GOTOFF:
  case R_386_GOTPC:
  case R_386_PC32:
  case R_386_PLT32:
    return read32le(Buf);
  }
}

void X86TargetInfo::relocateOne(uint8_t *Loc, uint32_t Type,
                                uint64_t Val) const {
  checkInt<32>(Val, Type);
  write32le(Loc, Val);
}

void X86TargetInfo::relaxTlsGdToLe(uint8_t *Loc, uint32_t Type,
                                   uint64_t Val) const {
  // Convert
  //   leal x@tlsgd(, %ebx, 1),
  //   call __tls_get_addr@plt
  // to
  //   movl %gs:0,%eax
  //   subl $x@ntpoff,%eax
  const uint8_t Inst[] = {
      0x65, 0xa1, 0x00, 0x00, 0x00, 0x00, // movl %gs:0, %eax
      0x81, 0xe8, 0x00, 0x00, 0x00, 0x00  // subl 0(%ebx), %eax
  };
  memcpy(Loc - 3, Inst, sizeof(Inst));
  relocateOne(Loc + 5, R_386_32, Val);
}

void X86TargetInfo::relaxTlsGdToIe(uint8_t *Loc, uint32_t Type,
                                   uint64_t Val) const {
  // Convert
  //   leal x@tlsgd(, %ebx, 1),
  //   call __tls_get_addr@plt
  // to
  //   movl %gs:0, %eax
  //   addl x@gotntpoff(%ebx), %eax
  const uint8_t Inst[] = {
      0x65, 0xa1, 0x00, 0x00, 0x00, 0x00, // movl %gs:0, %eax
      0x03, 0x83, 0x00, 0x00, 0x00, 0x00  // addl 0(%ebx), %eax
  };
  memcpy(Loc - 3, Inst, sizeof(Inst));
  relocateOne(Loc + 5, R_386_32, Val);
}

// In some conditions, relocations can be optimized to avoid using GOT.
// This function does that for Initial Exec to Local Exec case.
void X86TargetInfo::relaxTlsIeToLe(uint8_t *Loc, uint32_t Type,
                                   uint64_t Val) const {
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
  relocateOne(Loc, R_386_TLS_LE, Val);
}

void X86TargetInfo::relaxTlsLdToLe(uint8_t *Loc, uint32_t Type,
                                   uint64_t Val) const {
  if (Type == R_386_TLS_LDO_32) {
    relocateOne(Loc, R_386_TLS_LE, Val);
    return;
  }

  // Convert
  //   leal foo(%reg),%eax
  //   call ___tls_get_addr
  // to
  //   movl %gs:0,%eax
  //   nop
  //   leal 0(%esi,1),%esi
  const uint8_t Inst[] = {
      0x65, 0xa1, 0x00, 0x00, 0x00, 0x00, // movl %gs:0,%eax
      0x90,                               // nop
      0x8d, 0x74, 0x26, 0x00              // leal 0(%esi,1),%esi
  };
  memcpy(Loc - 2, Inst, sizeof(Inst));
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
  PltEntrySize = 16;
  PltZeroSize = 16;
  TlsGdRelaxSkip = 2;
}

RelExpr X86_64TargetInfo::getRelExpr(uint32_t Type, const SymbolBody &S) const {
  switch (Type) {
  default:
    return R_ABS;
  case R_X86_64_TPOFF32:
    return R_TLS;
  case R_X86_64_TLSLD:
    return R_TLSLD_PC;
  case R_X86_64_TLSGD:
    return R_TLSGD_PC;
  case R_X86_64_SIZE32:
  case R_X86_64_SIZE64:
    return R_SIZE;
  case R_X86_64_PLT32:
    return R_PLT_PC;
  case R_X86_64_PC32:
  case R_X86_64_PC64:
    return R_PC;
  case R_X86_64_GOT32:
    return R_GOT_FROM_END;
  case R_X86_64_GOTPCREL:
  case R_X86_64_GOTPCRELX:
  case R_X86_64_REX_GOTPCRELX:
  case R_X86_64_GOTTPOFF:
    return R_GOT_PC;
  }
}

void X86_64TargetInfo::writeGotPltHeader(uint8_t *Buf) const {
  // The first entry holds the value of _DYNAMIC. It is not clear why that is
  // required, but it is documented in the psabi and the glibc dynamic linker
  // seems to use it (note that this is relevant for linking ld.so, not any
  // other program).
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

uint32_t X86_64TargetInfo::getDynRel(uint32_t Type) const {
  if (Type == R_X86_64_PC32 || Type == R_X86_64_32)
    error(getRelName(Type) + " cannot be a dynamic relocation");
  return Type;
}

bool X86_64TargetInfo::isTlsInitialExecRel(uint32_t Type) const {
  return Type == R_X86_64_GOTTPOFF;
}

bool X86_64TargetInfo::isTlsGlobalDynamicRel(uint32_t Type) const {
  return Type == R_X86_64_TLSGD;
}

bool X86_64TargetInfo::isTlsLocalDynamicRel(uint32_t Type) const {
  return Type == R_X86_64_DTPOFF32 || Type == R_X86_64_DTPOFF64 ||
         Type == R_X86_64_TLSLD;
}

void X86_64TargetInfo::relaxTlsGdToLe(uint8_t *Loc, uint32_t Type,
                                      uint64_t Val) const {
  // Convert
  //   .byte 0x66
  //   leaq x@tlsgd(%rip), %rdi
  //   .word 0x6666
  //   rex64
  //   call __tls_get_addr@plt
  // to
  //   mov %fs:0x0,%rax
  //   lea x@tpoff,%rax
  const uint8_t Inst[] = {
      0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, // mov %fs:0x0,%rax
      0x48, 0x8d, 0x80, 0x00, 0x00, 0x00, 0x00              // lea x@tpoff,%rax
  };
  memcpy(Loc - 4, Inst, sizeof(Inst));
  // The original code used a pc relative relocation and so we have to
  // compensate for the -4 in had in the addend.
  relocateOne(Loc + 8, R_X86_64_TPOFF32, Val + 4);
}

void X86_64TargetInfo::relaxTlsGdToIe(uint8_t *Loc, uint32_t Type,
                                      uint64_t Val) const {
  // Convert
  //   .byte 0x66
  //   leaq x@tlsgd(%rip), %rdi
  //   .word 0x6666
  //   rex64
  //   call __tls_get_addr@plt
  // to
  //   mov %fs:0x0,%rax
  //   addq x@tpoff,%rax
  const uint8_t Inst[] = {
      0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, // mov %fs:0x0,%rax
      0x48, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00              // addq x@tpoff,%rax
  };
  memcpy(Loc - 4, Inst, sizeof(Inst));
  // Both code sequences are PC relatives, but since we are moving the constant
  // forward by 8 bytes we have to subtract the value by 8.
  relocateOne(Loc + 8, R_X86_64_PC32, Val - 8);
}

// In some conditions, R_X86_64_GOTTPOFF relocation can be optimized to
// R_X86_64_TPOFF32 so that it does not use GOT.
void X86_64TargetInfo::relaxTlsIeToLe(uint8_t *Loc, uint32_t Type,
                                      uint64_t Val) const {
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
  // The original code used a pc relative relocation and so we have to
  // compensate for the -4 in had in the addend.
  relocateOne(Loc, R_X86_64_TPOFF32, Val + 4);
}

void X86_64TargetInfo::relaxTlsLdToLe(uint8_t *Loc, uint32_t Type,
                                      uint64_t Val) const {
  // Convert
  //   leaq bar@tlsld(%rip), %rdi
  //   callq __tls_get_addr@PLT
  //   leaq bar@dtpoff(%rax), %rcx
  // to
  //   .word 0x6666
  //   .byte 0x66
  //   mov %fs:0,%rax
  //   leaq bar@tpoff(%rax), %rcx
  if (Type == R_X86_64_DTPOFF64) {
    write64le(Loc, Val);
    return;
  }
  if (Type == R_X86_64_DTPOFF32) {
    relocateOne(Loc, R_X86_64_TPOFF32, Val);
    return;
  }

  const uint8_t Inst[] = {
      0x66, 0x66,                                          // .word 0x6666
      0x66,                                                // .byte 0x66
      0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00 // mov %fs:0,%rax
  };
  memcpy(Loc - 3, Inst, sizeof(Inst));
}

void X86_64TargetInfo::relocateOne(uint8_t *Loc, uint32_t Type,
                                   uint64_t Val) const {
  switch (Type) {
  case R_X86_64_32:
    checkUInt<32>(Val, Type);
    write32le(Loc, Val);
    break;
  case R_X86_64_32S:
  case R_X86_64_TPOFF32:
  case R_X86_64_GOT32:
  case R_X86_64_GOTPCREL:
  case R_X86_64_GOTPCRELX:
  case R_X86_64_REX_GOTPCRELX:
  case R_X86_64_PC32:
  case R_X86_64_GOTTPOFF:
  case R_X86_64_PLT32:
  case R_X86_64_TLSGD:
  case R_X86_64_TLSLD:
  case R_X86_64_DTPOFF32:
  case R_X86_64_SIZE32:
    checkInt<32>(Val, Type);
    write32le(Loc, Val);
    break;
  case R_X86_64_64:
  case R_X86_64_DTPOFF64:
  case R_X86_64_SIZE64:
  case R_X86_64_PC64:
    write64le(Loc, Val);
    break;
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

RelExpr X86_64TargetInfo::adjustRelaxExpr(uint32_t Type, const uint8_t *Data,
                                          RelExpr RelExpr) const {
  if (Type != R_X86_64_GOTPCRELX && Type != R_X86_64_REX_GOTPCRELX)
    return RelExpr;
  const uint8_t Op = Data[-2];
  const uint8_t ModRm = Data[-1];
  // FIXME: When PIC is disabled and foo is defined locally in the
  // lower 32 bit address space, memory operand in mov can be converted into
  // immediate operand. Otherwise, mov must be changed to lea. We support only
  // latter relaxation at this moment.
  if (Op == 0x8b)
    return R_RELAX_GOT_PC;
  // Relax call and jmp.
  if (Op == 0xff && (ModRm == 0x15 || ModRm == 0x25))
    return R_RELAX_GOT_PC;

  // Relaxation of test, adc, add, and, cmp, or, sbb, sub, xor.
  // If PIC then no relaxation is available.
  // We also don't relax test/binop instructions without REX byte,
  // they are 32bit operations and not common to have.
  assert(Type == R_X86_64_REX_GOTPCRELX);
  return Config->Pic ? RelExpr : R_RELAX_GOT_PC_NOPIC;
}

// A subset of relaxations can only be applied for no-PIC. This method
// handles such relaxations. Instructions encoding information was taken from:
// "Intel 64 and IA-32 Architectures Software Developer's Manual V2"
// (http://www.intel.com/content/dam/www/public/us/en/documents/manuals/
//    64-ia-32-architectures-software-developer-instruction-set-reference-manual-325383.pdf)
void X86_64TargetInfo::relaxGotNoPic(uint8_t *Loc, uint64_t Val, uint8_t Op,
                                     uint8_t ModRm) const {
  const uint8_t Rex = Loc[-3];
  // Convert "test %reg, foo@GOTPCREL(%rip)" to "test $foo, %reg".
  if (Op == 0x85) {
    // See "TEST-Logical Compare" (4-428 Vol. 2B),
    // TEST r/m64, r64 uses "full" ModR / M byte (no opcode extension).

    // ModR/M byte has form XX YYY ZZZ, where
    // YYY is MODRM.reg(register 2), ZZZ is MODRM.rm(register 1).
    // XX has different meanings:
    // 00: The operand's memory address is in reg1.
    // 01: The operand's memory address is reg1 + a byte-sized displacement.
    // 10: The operand's memory address is reg1 + a word-sized displacement.
    // 11: The operand is reg1 itself.
    // If an instruction requires only one operand, the unused reg2 field
    // holds extra opcode bits rather than a register code
    // 0xC0 == 11 000 000 binary.
    // 0x38 == 00 111 000 binary.
    // We transfer reg2 to reg1 here as operand.
    // See "2.1.3 ModR/M and SIB Bytes" (Vol. 2A 2-3).
    *(Loc - 1) = 0xc0 | (ModRm & 0x38) >> 3; // ModR/M byte.

    // Change opcode from TEST r/m64, r64 to TEST r/m64, imm32
    // See "TEST-Logical Compare" (4-428 Vol. 2B).
    *(Loc - 2) = 0xf7;

    // Move R bit to the B bit in REX byte.
    // REX byte is encoded as 0100WRXB, where
    // 0100 is 4bit fixed pattern.
    // REX.W When 1, a 64-bit operand size is used. Otherwise, when 0, the
    //   default operand size is used (which is 32-bit for most but not all
    //   instructions).
    // REX.R This 1-bit value is an extension to the MODRM.reg field.
    // REX.X This 1-bit value is an extension to the SIB.index field.
    // REX.B This 1-bit value is an extension to the MODRM.rm field or the
    // SIB.base field.
    // See "2.2.1.2 More on REX Prefix Fields " (2-8 Vol. 2A).
    *(Loc - 3) = (Rex & ~0x4) | (Rex & 0x4) >> 2;
    relocateOne(Loc, R_X86_64_PC32, Val);
    return;
  }

  // If we are here then we need to relax the adc, add, and, cmp, or, sbb, sub
  // or xor operations.

  // Convert "binop foo@GOTPCREL(%rip), %reg" to "binop $foo, %reg".
  // Logic is close to one for test instruction above, but we also
  // write opcode extension here, see below for details.
  *(Loc - 1) = 0xc0 | (ModRm & 0x38) >> 3 | (Op & 0x3c); // ModR/M byte.

  // Primary opcode is 0x81, opcode extension is one of:
  // 000b = ADD, 001b is OR, 010b is ADC, 011b is SBB,
  // 100b is AND, 101b is SUB, 110b is XOR, 111b is CMP.
  // This value was wrote to MODRM.reg in a line above.
  // See "3.2 INSTRUCTIONS (A-M)" (Vol. 2A 3-15),
  // "INSTRUCTION SET REFERENCE, N-Z" (Vol. 2B 4-1) for
  // descriptions about each operation.
  *(Loc - 2) = 0x81;
  *(Loc - 3) = (Rex & ~0x4) | (Rex & 0x4) >> 2;
  relocateOne(Loc, R_X86_64_PC32, Val);
}

void X86_64TargetInfo::relaxGot(uint8_t *Loc, uint64_t Val) const {
  const uint8_t Op = Loc[-2];
  const uint8_t ModRm = Loc[-1];

  // Convert mov foo@GOTPCREL(%rip), %reg to lea foo(%rip), %reg.
  if (Op == 0x8b) {
    *(Loc - 2) = 0x8d;
    relocateOne(Loc, R_X86_64_PC32, Val);
    return;
  }

  // Convert call/jmp instructions.
  if (Op == 0xff) {
    if (ModRm == 0x15) {
      // ABI says we can convert call *foo@GOTPCREL(%rip) to nop call foo.
      // Instead we convert to addr32 call foo, where addr32 is instruction
      // prefix. That makes result expression to be a single instruction.
      *(Loc - 2) = 0x67; // addr32 prefix
      *(Loc - 1) = 0xe8; // call
    } else {
      assert(ModRm == 0x25);
      // Convert jmp *foo@GOTPCREL(%rip) to jmp foo nop.
      // jmp doesn't return, so it is fine to use nop here, it is just a stub.
      *(Loc - 2) = 0xe9; // jmp
      *(Loc + 3) = 0x90; // nop
      Loc -= 1;
      Val += 1;
    }
    relocateOne(Loc, R_X86_64_PC32, Val);
    return;
  }

  assert(!Config->Pic);
  // We are relaxing a rip relative to an absolute, so compensate
  // for the old -4 addend.
  relaxGotNoPic(Loc, Val + 4, Op, ModRm);
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

void PPCTargetInfo::relocateOne(uint8_t *Loc, uint32_t Type,
                                uint64_t Val) const {
  switch (Type) {
  case R_PPC_ADDR16_HA:
    write16be(Loc, applyPPCHa(Val));
    break;
  case R_PPC_ADDR16_LO:
    write16be(Loc, applyPPCLo(Val));
    break;
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

RelExpr PPCTargetInfo::getRelExpr(uint32_t Type, const SymbolBody &S) const {
  return R_ABS;
}

PPC64TargetInfo::PPC64TargetInfo() {
  PltRel = GotRel = R_PPC64_GLOB_DAT;
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

static uint64_t PPC64TocOffset = 0x8000;

uint64_t getPPC64TocBase() {
  // The TOC consists of sections .got, .toc, .tocbss, .plt in that order. The
  // TOC starts where the first of these sections starts. We always create a
  // .got when we see a relocation that uses it, so for us the start is always
  // the .got.
  uint64_t TocVA = Out<ELF64BE>::Got->getVA();

  // Per the ppc64-elf-linux ABI, The TOC base is TOC value plus 0x8000
  // thus permitting a full 64 Kbytes segment. Note that the glibc startup
  // code (crt1.o) assumes that you can get from the TOC base to the
  // start of the .toc section with only a single (signed) 16-bit relocation.
  return TocVA + PPC64TocOffset;
}

RelExpr PPC64TargetInfo::getRelExpr(uint32_t Type, const SymbolBody &S) const {
  switch (Type) {
  default:
    return R_ABS;
  case R_PPC64_TOC16:
  case R_PPC64_TOC16_DS:
  case R_PPC64_TOC16_HA:
  case R_PPC64_TOC16_HI:
  case R_PPC64_TOC16_LO:
  case R_PPC64_TOC16_LO_DS:
    return R_GOTREL;
  case R_PPC64_TOC:
    return R_PPC_TOC;
  case R_PPC64_REL24:
    return R_PPC_PLT_OPD;
  }
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

void PPC64TargetInfo::relocateOne(uint8_t *Loc, uint32_t Type,
                                  uint64_t Val) const {
  uint64_t TO = PPC64TocOffset;

  // For a TOC-relative relocation,  proceed in terms of the corresponding
  // ADDR16 relocation type.
  switch (Type) {
  case R_PPC64_TOC16:       Type = R_PPC64_ADDR16;       Val -= TO; break;
  case R_PPC64_TOC16_DS:    Type = R_PPC64_ADDR16_DS;    Val -= TO; break;
  case R_PPC64_TOC16_HA:    Type = R_PPC64_ADDR16_HA;    Val -= TO; break;
  case R_PPC64_TOC16_HI:    Type = R_PPC64_ADDR16_HI;    Val -= TO; break;
  case R_PPC64_TOC16_LO:    Type = R_PPC64_ADDR16_LO;    Val -= TO; break;
  case R_PPC64_TOC16_LO_DS: Type = R_PPC64_ADDR16_LO_DS; Val -= TO; break;
  default: break;
  }

  switch (Type) {
  case R_PPC64_ADDR14: {
    checkAlignment<4>(Val, Type);
    // Preserve the AA/LK bits in the branch instruction
    uint8_t AALK = Loc[3];
    write16be(Loc + 2, (AALK & 3) | (Val & 0xfffc));
    break;
  }
  case R_PPC64_ADDR16:
    checkInt<16>(Val, Type);
    write16be(Loc, Val);
    break;
  case R_PPC64_ADDR16_DS:
    checkInt<16>(Val, Type);
    write16be(Loc, (read16be(Loc) & 3) | (Val & ~3));
    break;
  case R_PPC64_ADDR16_HA:
    write16be(Loc, applyPPCHa(Val));
    break;
  case R_PPC64_ADDR16_HI:
    write16be(Loc, applyPPCHi(Val));
    break;
  case R_PPC64_ADDR16_HIGHER:
    write16be(Loc, applyPPCHigher(Val));
    break;
  case R_PPC64_ADDR16_HIGHERA:
    write16be(Loc, applyPPCHighera(Val));
    break;
  case R_PPC64_ADDR16_HIGHEST:
    write16be(Loc, applyPPCHighest(Val));
    break;
  case R_PPC64_ADDR16_HIGHESTA:
    write16be(Loc, applyPPCHighesta(Val));
    break;
  case R_PPC64_ADDR16_LO:
    write16be(Loc, applyPPCLo(Val));
    break;
  case R_PPC64_ADDR16_LO_DS:
    write16be(Loc, (read16be(Loc) & 3) | (applyPPCLo(Val) & ~3));
    break;
  case R_PPC64_ADDR32:
    checkInt<32>(Val, Type);
    write32be(Loc, Val);
    break;
  case R_PPC64_ADDR64:
    write64be(Loc, Val);
    break;
  case R_PPC64_REL16_HA:
    write16be(Loc, applyPPCHa(Val));
    break;
  case R_PPC64_REL16_HI:
    write16be(Loc, applyPPCHi(Val));
    break;
  case R_PPC64_REL16_LO:
    write16be(Loc, applyPPCLo(Val));
    break;
  case R_PPC64_REL24: {
    uint32_t Mask = 0x03FFFFFC;
    checkInt<24>(Val, Type);
    write32be(Loc, (read32be(Loc) & ~Mask) | (Val & Mask));
    break;
  }
  case R_PPC64_REL32:
    checkInt<32>(Val, Type);
    write32be(Loc, Val);
    break;
  case R_PPC64_REL64:
    write64be(Loc, Val);
    break;
  case R_PPC64_TOC:
    write64be(Loc, Val);
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
  TlsDescRel = R_AARCH64_TLSDESC;
  TlsGotRel = R_AARCH64_TLS_TPREL64;
  PltEntrySize = 16;
  PltZeroSize = 32;

  // It doesn't seem to be documented anywhere, but tls on aarch64 uses variant
  // 1 of the tls structures and the tcb size is 16.
  TcbSize = 16;
}

RelExpr AArch64TargetInfo::getRelExpr(uint32_t Type,
                                      const SymbolBody &S) const {
  switch (Type) {
  default:
    return R_ABS;

  case R_AARCH64_TLSDESC_ADR_PAGE21:
    return R_TLSDESC_PAGE;

  case R_AARCH64_TLSDESC_LD64_LO12_NC:
  case R_AARCH64_TLSDESC_ADD_LO12_NC:
    return R_TLSDESC;

  case R_AARCH64_TLSDESC_CALL:
    return R_HINT;

  case R_AARCH64_TLSLE_ADD_TPREL_HI12:
  case R_AARCH64_TLSLE_ADD_TPREL_LO12_NC:
    return R_TLS;

  case R_AARCH64_CALL26:
  case R_AARCH64_CONDBR19:
  case R_AARCH64_JUMP26:
  case R_AARCH64_TSTBR14:
    return R_PLT_PC;

  case R_AARCH64_PREL16:
  case R_AARCH64_PREL32:
  case R_AARCH64_PREL64:
  case R_AARCH64_ADR_PREL_LO21:
    return R_PC;
  case R_AARCH64_ADR_PREL_PG_HI21:
    return R_PAGE_PC;
  case R_AARCH64_LD64_GOT_LO12_NC:
  case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC:
    return R_GOT;
  case R_AARCH64_ADR_GOT_PAGE:
  case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21:
    return R_GOT_PAGE_PC;
  }
}

RelExpr AArch64TargetInfo::adjustRelaxExpr(uint32_t Type, const uint8_t *Data,
                                           RelExpr Expr) const {
  if (Expr == R_RELAX_TLS_GD_TO_IE) {
    if (Type == R_AARCH64_TLSDESC_ADR_PAGE21)
      return R_RELAX_TLS_GD_TO_IE_PAGE_PC;
    return R_RELAX_TLS_GD_TO_IE_ABS;
  }
  return Expr;
}

bool AArch64TargetInfo::usesOnlyLowPageBits(uint32_t Type) const {
  switch (Type) {
  default:
    return false;
  case R_AARCH64_ADD_ABS_LO12_NC:
  case R_AARCH64_LD64_GOT_LO12_NC:
  case R_AARCH64_LDST128_ABS_LO12_NC:
  case R_AARCH64_LDST16_ABS_LO12_NC:
  case R_AARCH64_LDST32_ABS_LO12_NC:
  case R_AARCH64_LDST64_ABS_LO12_NC:
  case R_AARCH64_LDST8_ABS_LO12_NC:
  case R_AARCH64_TLSDESC_ADD_LO12_NC:
  case R_AARCH64_TLSDESC_LD64_LO12_NC:
  case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC:
    return true;
  }
}

bool AArch64TargetInfo::isTlsInitialExecRel(uint32_t Type) const {
  return Type == R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 ||
         Type == R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC;
}

uint32_t AArch64TargetInfo::getDynRel(uint32_t Type) const {
  if (Type == R_AARCH64_ABS32 || Type == R_AARCH64_ABS64)
    return Type;
  // Keep it going with a dummy value so that we can find more reloc errors.
  errorDynRel(Type);
  return R_AARCH64_ABS32;
}

void AArch64TargetInfo::writeGotPlt(uint8_t *Buf, uint64_t Plt) const {
  write64le(Buf, Out<ELF64LE>::Plt->getVA());
}

static uint64_t getAArch64Page(uint64_t Expr) {
  return Expr & (~static_cast<uint64_t>(0xFFF));
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
  relocateOne(Buf + 4, R_AARCH64_ADR_PREL_PG_HI21,
              getAArch64Page(Got + 16) - getAArch64Page(Plt + 4));
  relocateOne(Buf + 8, R_AARCH64_LDST64_ABS_LO12_NC, Got + 16);
  relocateOne(Buf + 12, R_AARCH64_ADD_ABS_LO12_NC, Got + 16);
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

  relocateOne(Buf, R_AARCH64_ADR_PREL_PG_HI21,
              getAArch64Page(GotEntryAddr) - getAArch64Page(PltEntryAddr));
  relocateOne(Buf + 4, R_AARCH64_LDST64_ABS_LO12_NC, GotEntryAddr);
  relocateOne(Buf + 8, R_AARCH64_ADD_ABS_LO12_NC, GotEntryAddr);
}

static void updateAArch64Addr(uint8_t *L, uint64_t Imm) {
  uint32_t ImmLo = (Imm & 0x3) << 29;
  uint32_t ImmHi = (Imm & 0x1FFFFC) << 3;
  uint64_t Mask = (0x3 << 29) | (0x1FFFFC << 3);
  write32le(L, (read32le(L) & ~Mask) | ImmLo | ImmHi);
}

static inline void updateAArch64Add(uint8_t *L, uint64_t Imm) {
  or32le(L, (Imm & 0xFFF) << 10);
}

void AArch64TargetInfo::relocateOne(uint8_t *Loc, uint32_t Type,
                                    uint64_t Val) const {
  switch (Type) {
  case R_AARCH64_ABS16:
  case R_AARCH64_PREL16:
    checkIntUInt<16>(Val, Type);
    write16le(Loc, Val);
    break;
  case R_AARCH64_ABS32:
  case R_AARCH64_PREL32:
    checkIntUInt<32>(Val, Type);
    write32le(Loc, Val);
    break;
  case R_AARCH64_ABS64:
  case R_AARCH64_PREL64:
    write64le(Loc, Val);
    break;
  case R_AARCH64_ADD_ABS_LO12_NC:
    // This relocation stores 12 bits and there's no instruction
    // to do it. Instead, we do a 32 bits store of the value
    // of r_addend bitwise-or'ed Loc. This assumes that the addend
    // bits in Loc are zero.
    or32le(Loc, (Val & 0xFFF) << 10);
    break;
  case R_AARCH64_ADR_GOT_PAGE:
  case R_AARCH64_ADR_PREL_PG_HI21:
  case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21:
  case R_AARCH64_TLSDESC_ADR_PAGE21:
    checkInt<33>(Val, Type);
    updateAArch64Addr(Loc, Val >> 12);
    break;
  case R_AARCH64_ADR_PREL_LO21:
    checkInt<21>(Val, Type);
    updateAArch64Addr(Loc, Val);
    break;
  case R_AARCH64_CALL26:
  case R_AARCH64_JUMP26:
    checkInt<28>(Val, Type);
    or32le(Loc, (Val & 0x0FFFFFFC) >> 2);
    break;
  case R_AARCH64_CONDBR19:
    checkInt<21>(Val, Type);
    or32le(Loc, (Val & 0x1FFFFC) << 3);
    break;
  case R_AARCH64_LD64_GOT_LO12_NC:
  case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC:
  case R_AARCH64_TLSDESC_LD64_LO12_NC:
    checkAlignment<8>(Val, Type);
    or32le(Loc, (Val & 0xFF8) << 7);
    break;
  case R_AARCH64_LDST128_ABS_LO12_NC:
    or32le(Loc, (Val & 0x0FF8) << 6);
    break;
  case R_AARCH64_LDST16_ABS_LO12_NC:
    or32le(Loc, (Val & 0x0FFC) << 9);
    break;
  case R_AARCH64_LDST8_ABS_LO12_NC:
    or32le(Loc, (Val & 0xFFF) << 10);
    break;
  case R_AARCH64_LDST32_ABS_LO12_NC:
    or32le(Loc, (Val & 0xFFC) << 8);
    break;
  case R_AARCH64_LDST64_ABS_LO12_NC:
    or32le(Loc, (Val & 0xFF8) << 7);
    break;
  case R_AARCH64_TSTBR14:
    checkInt<16>(Val, Type);
    or32le(Loc, (Val & 0xFFFC) << 3);
    break;
  case R_AARCH64_TLSLE_ADD_TPREL_HI12:
    checkInt<24>(Val, Type);
    updateAArch64Add(Loc, Val >> 12);
    break;
  case R_AARCH64_TLSLE_ADD_TPREL_LO12_NC:
  case R_AARCH64_TLSDESC_ADD_LO12_NC:
    updateAArch64Add(Loc, Val);
    break;
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

void AArch64TargetInfo::relaxTlsGdToLe(uint8_t *Loc, uint32_t Type,
                                       uint64_t Val) const {
  // TLSDESC Global-Dynamic relocation are in the form:
  //   adrp    x0, :tlsdesc:v             [R_AARCH64_TLSDESC_ADR_PAGE21]
  //   ldr     x1, [x0, #:tlsdesc_lo12:v  [R_AARCH64_TLSDESC_LD64_LO12_NC]
  //   add     x0, x0, :tlsdesc_los:v     [_AARCH64_TLSDESC_ADD_LO12_NC]
  //   .tlsdesccall                       [R_AARCH64_TLSDESC_CALL]
  //   blr     x1
  // And it can optimized to:
  //   movz    x0, #0x0, lsl #16
  //   movk    x0, #0x10
  //   nop
  //   nop
  checkUInt<32>(Val, Type);

  uint32_t NewInst;
  switch (Type) {
  case R_AARCH64_TLSDESC_ADD_LO12_NC:
  case R_AARCH64_TLSDESC_CALL:
    // nop
    NewInst = 0xd503201f;
    break;
  case R_AARCH64_TLSDESC_ADR_PAGE21:
    // movz
    NewInst = 0xd2a00000 | (((Val >> 16) & 0xffff) << 5);
    break;
  case R_AARCH64_TLSDESC_LD64_LO12_NC:
    // movk
    NewInst = 0xf2800000 | ((Val & 0xffff) << 5);
    break;
  default:
    llvm_unreachable("unsupported Relocation for TLS GD to LE relax");
  }
  write32le(Loc, NewInst);
}

void AArch64TargetInfo::relaxTlsGdToIe(uint8_t *Loc, uint32_t Type,
                                       uint64_t Val) const {
  // TLSDESC Global-Dynamic relocation are in the form:
  //   adrp    x0, :tlsdesc:v             [R_AARCH64_TLSDESC_ADR_PAGE21]
  //   ldr     x1, [x0, #:tlsdesc_lo12:v  [R_AARCH64_TLSDESC_LD64_LO12_NC]
  //   add     x0, x0, :tlsdesc_los:v     [_AARCH64_TLSDESC_ADD_LO12_NC]
  //   .tlsdesccall                       [R_AARCH64_TLSDESC_CALL]
  //   blr     x1
  // And it can optimized to:
  //   adrp    x0, :gottprel:v
  //   ldr     x0, [x0, :gottprel_lo12:v]
  //   nop
  //   nop

  switch (Type) {
  case R_AARCH64_TLSDESC_ADD_LO12_NC:
  case R_AARCH64_TLSDESC_CALL:
    write32le(Loc, 0xd503201f); // nop
    break;
  case R_AARCH64_TLSDESC_ADR_PAGE21:
    write32le(Loc, 0x90000000); // adrp
    relocateOne(Loc, R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21, Val);
    break;
  case R_AARCH64_TLSDESC_LD64_LO12_NC:
    write32le(Loc, 0xf9400000); // ldr
    relocateOne(Loc, R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC, Val);
    break;
  default:
    llvm_unreachable("unsupported Relocation for TLS GD to LE relax");
  }
}

void AArch64TargetInfo::relaxTlsIeToLe(uint8_t *Loc, uint32_t Type,
                                       uint64_t Val) const {
  checkUInt<32>(Val, Type);

  uint32_t Inst = read32le(Loc);
  uint32_t NewInst;
  if (Type == R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21) {
    // Generate movz.
    unsigned RegNo = (Inst & 0x1f);
    NewInst = (0xd2a00000 | RegNo) | (((Val >> 16) & 0xffff) << 5);
  } else if (Type == R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC) {
    // Generate movk
    unsigned RegNo = (Inst & 0x1f);
    NewInst = (0xf2800000 | RegNo) | ((Val & 0xffff) << 5);
  } else {
    llvm_unreachable("invalid Relocation for TLS IE to LE Relax");
  }
  write32le(Loc, NewInst);
}

// Implementing relocations for AMDGPU is low priority since most
// programs don't use relocations now. Thus, this function is not
// actually called (relocateOne is called for each relocation).
// That's why the AMDGPU port works without implementing this function.
void AMDGPUTargetInfo::relocateOne(uint8_t *Loc, uint32_t Type,
                                   uint64_t Val) const {
  llvm_unreachable("not implemented");
}

RelExpr AMDGPUTargetInfo::getRelExpr(uint32_t Type, const SymbolBody &S) const {
  llvm_unreachable("not implemented");
}

ARMTargetInfo::ARMTargetInfo() {
  CopyRel = R_ARM_COPY;
  RelativeRel = R_ARM_RELATIVE;
  IRelativeRel = R_ARM_IRELATIVE;
  GotRel = R_ARM_GLOB_DAT;
  PltRel = R_ARM_JUMP_SLOT;
  TlsGotRel = R_ARM_TLS_TPOFF32;
  TlsModuleIndexRel = R_ARM_TLS_DTPMOD32;
  TlsOffsetRel = R_ARM_TLS_DTPOFF32;
  PltEntrySize = 16;
  PltZeroSize = 20;
}

RelExpr ARMTargetInfo::getRelExpr(uint32_t Type, const SymbolBody &S) const {
  switch (Type) {
  default:
    return R_ABS;
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PC24:
  case R_ARM_PLT32:
    return R_PLT_PC;
  case R_ARM_GOTOFF32:
    // (S + A) - GOT_ORG
    return R_GOTREL;
  case R_ARM_GOT_BREL:
    // GOT(S) + A - GOT_ORG
    return R_GOT_OFF;
  case R_ARM_GOT_PREL:
    // GOT(S) + - GOT_ORG
    return R_GOT_PC;
  case R_ARM_BASE_PREL:
    // B(S) + A - P
    // FIXME: currently B(S) assumed to be .got, this may not hold for all
    // platforms.
    return R_GOTONLY_PC;
  case R_ARM_PREL31:
  case R_ARM_REL32:
    return R_PC;
  }
}

uint32_t ARMTargetInfo::getDynRel(uint32_t Type) const {
  if (Type == R_ARM_ABS32)
    return Type;
  // Keep it going with a dummy value so that we can find more reloc errors.
  errorDynRel(Type);
  return R_ARM_ABS32;
}

void ARMTargetInfo::writeGotPlt(uint8_t *Buf, uint64_t Plt) const {
  write32le(Buf, Out<ELF32LE>::Plt->getVA());
}

void ARMTargetInfo::writePltZero(uint8_t *Buf) const {
  const uint8_t PltData[] = {
      0x04, 0xe0, 0x2d, 0xe5, //     str lr, [sp,#-4]!
      0x04, 0xe0, 0x9f, 0xe5, //     ldr lr, L2
      0x0e, 0xe0, 0x8f, 0xe0, // L1: add lr, pc, lr
      0x08, 0xf0, 0xbe, 0xe5, //     ldr pc, [lr, #8]
      0x00, 0x00, 0x00, 0x00, // L2: .word   &(.got.plt) - L1 - 8
  };
  memcpy(Buf, PltData, sizeof(PltData));
  uint64_t GotPlt = Out<ELF32LE>::GotPlt->getVA();
  uint64_t L1 = Out<ELF32LE>::Plt->getVA() + 8;
  write32le(Buf + 16, GotPlt - L1 - 8);
}

void ARMTargetInfo::writePlt(uint8_t *Buf, uint64_t GotEntryAddr,
                             uint64_t PltEntryAddr, int32_t Index,
                             unsigned RelOff) const {
  // FIXME: Using simple code sequence with simple relocations.
  // There is a more optimal sequence but it requires support for the group
  // relocations. See ELF for the ARM Architecture Appendix A.3
  const uint8_t PltData[] = {
      0x04, 0xc0, 0x9f, 0xe5, //     ldr ip, L2
      0x0f, 0xc0, 0x8c, 0xe0, // L1: add ip, ip, pc
      0x00, 0xf0, 0x9c, 0xe5, //     ldr pc, [ip]
      0x00, 0x00, 0x00, 0x00, // L2: .word   Offset(&(.plt.got) - L1 - 8
  };
  memcpy(Buf, PltData, sizeof(PltData));
  uint64_t L1 = PltEntryAddr + 4;
  write32le(Buf + 12, GotEntryAddr - L1 - 8);
}

void ARMTargetInfo::relocateOne(uint8_t *Loc, uint32_t Type,
                                uint64_t Val) const {
  switch (Type) {
  case R_ARM_NONE:
    break;
  case R_ARM_ABS32:
  case R_ARM_BASE_PREL:
  case R_ARM_GOTOFF32:
  case R_ARM_GOT_BREL:
  case R_ARM_GOT_PREL:
  case R_ARM_REL32:
    write32le(Loc, Val);
    break;
  case R_ARM_PREL31:
    checkInt<31>(Val, Type);
    write32le(Loc, (read32le(Loc) & 0x80000000) | (Val & ~0x80000000));
    break;
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PC24:
  case R_ARM_PLT32:
    checkInt<26>(Val, Type);
    write32le(Loc, (read32le(Loc) & ~0x00ffffff) | ((Val >> 2) & 0x00ffffff));
    break;
  case R_ARM_MOVW_ABS_NC:
    write32le(Loc, (read32le(Loc) & ~0x000f0fff) | ((Val & 0xf000) << 4) |
                       (Val & 0x0fff));
    break;
  case R_ARM_MOVT_ABS:
    checkUInt<32>(Val, Type);
    write32le(Loc, (read32le(Loc) & ~0x000f0fff) |
                       (((Val >> 16) & 0xf000) << 4) | ((Val >> 16) & 0xfff));
    break;
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

uint64_t ARMTargetInfo::getImplicitAddend(const uint8_t *Buf,
                                          uint32_t Type) const {
  switch (Type) {
  default:
    return 0;
  case R_ARM_ABS32:
  case R_ARM_BASE_PREL:
  case R_ARM_GOTOFF32:
  case R_ARM_GOT_BREL:
  case R_ARM_GOT_PREL:
  case R_ARM_REL32:
    return SignExtend64<32>(read32le(Buf));
  case R_ARM_PREL31:
    return SignExtend64<31>(read32le(Buf));
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PC24:
  case R_ARM_PLT32:
    return SignExtend64<26>((read32le(Buf) & 0x00ffffff) << 2);
  case R_ARM_MOVW_ABS_NC:
  case R_ARM_MOVT_ABS: {
    // ELF for the ARM Architecture 4.6.1.1 the implicit addend for MOVW and
    // MOVT is in the range -32768 <= A < 32768
    uint64_t Val = read32le(Buf) & 0x000f0fff;
    return SignExtend64<16>(((Val & 0x000f0000) >> 4) | (Val & 0x00fff));
  }
  }
}

template <class ELFT> MipsTargetInfo<ELFT>::MipsTargetInfo() {
  GotPltHeaderEntriesNum = 2;
  PageSize = 65536;
  PltEntrySize = 16;
  PltZeroSize = 32;
  ThunkSize = 16;
  CopyRel = R_MIPS_COPY;
  PltRel = R_MIPS_JUMP_SLOT;
  if (ELFT::Is64Bits)
    RelativeRel = (R_MIPS_64 << 8) | R_MIPS_REL32;
  else
    RelativeRel = R_MIPS_REL32;
}

template <class ELFT>
RelExpr MipsTargetInfo<ELFT>::getRelExpr(uint32_t Type,
                                         const SymbolBody &S) const {
  if (ELFT::Is64Bits)
    // See comment in the calculateMips64RelChain.
    Type &= 0xff;
  switch (Type) {
  default:
    return R_ABS;
  case R_MIPS_JALR:
    return R_HINT;
  case R_MIPS_GPREL16:
  case R_MIPS_GPREL32:
    return R_GOTREL;
  case R_MIPS_26:
    return R_PLT;
  case R_MIPS_HI16:
  case R_MIPS_LO16:
  case R_MIPS_GOT_OFST:
    // MIPS _gp_disp designates offset between start of function and 'gp'
    // pointer into GOT. __gnu_local_gp is equal to the current value of
    // the 'gp'. Therefore any relocations against them do not require
    // dynamic relocation.
    if (&S == ElfSym<ELFT>::MipsGpDisp)
      return R_PC;
    return R_ABS;
  case R_MIPS_PC32:
  case R_MIPS_PC16:
  case R_MIPS_PC19_S2:
  case R_MIPS_PC21_S2:
  case R_MIPS_PC26_S2:
  case R_MIPS_PCHI16:
  case R_MIPS_PCLO16:
    return R_PC;
  case R_MIPS_GOT16:
    if (S.isLocal())
      return R_MIPS_GOT_LOCAL_PAGE;
  // fallthrough
  case R_MIPS_CALL16:
  case R_MIPS_GOT_DISP:
    if (!S.isPreemptible())
      return R_MIPS_GOT_LOCAL;
    return R_GOT_OFF;
  case R_MIPS_GOT_PAGE:
    return R_MIPS_GOT_LOCAL_PAGE;
  }
}

template <class ELFT>
uint32_t MipsTargetInfo<ELFT>::getDynRel(uint32_t Type) const {
  if (Type == R_MIPS_32 || Type == R_MIPS_64)
    return RelativeRel;
  // Keep it going with a dummy value so that we can find more reloc errors.
  errorDynRel(Type);
  return R_MIPS_32;
}

template <class ELFT>
void MipsTargetInfo<ELFT>::writeGotPlt(uint8_t *Buf, uint64_t Plt) const {
  write32<ELFT::TargetEndianness>(Buf, Out<ELFT>::Plt->getVA());
}

static uint16_t mipsHigh(uint64_t V) { return (V + 0x8000) >> 16; }

template <endianness E, uint8_t BSIZE, uint8_t SHIFT>
static int64_t getPcRelocAddend(const uint8_t *Loc) {
  uint32_t Instr = read32<E>(Loc);
  uint32_t Mask = 0xffffffff >> (32 - BSIZE);
  return SignExtend64<BSIZE + SHIFT>((Instr & Mask) << SHIFT);
}

template <endianness E, uint8_t BSIZE, uint8_t SHIFT>
static void applyMipsPcReloc(uint8_t *Loc, uint32_t Type, uint64_t V) {
  uint32_t Mask = 0xffffffff >> (32 - BSIZE);
  uint32_t Instr = read32<E>(Loc);
  if (SHIFT > 0)
    checkAlignment<(1 << SHIFT)>(V, Type);
  checkInt<BSIZE + SHIFT>(V, Type);
  write32<E>(Loc, (Instr & ~Mask) | ((V >> SHIFT) & Mask));
}

template <endianness E>
static void writeMipsHi16(uint8_t *Loc, uint64_t V) {
  uint32_t Instr = read32<E>(Loc);
  write32<E>(Loc, (Instr & 0xffff0000) | mipsHigh(V));
}

template <endianness E>
static void writeMipsLo16(uint8_t *Loc, uint64_t V) {
  uint32_t Instr = read32<E>(Loc);
  write32<E>(Loc, (Instr & 0xffff0000) | (V & 0xffff));
}

template <endianness E> static int16_t readSignedLo16(const uint8_t *Loc) {
  return SignExtend32<16>(read32<E>(Loc) & 0xffff);
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
  writeMipsHi16<E>(Buf, Got);
  writeMipsLo16<E>(Buf + 4, Got);
  writeMipsLo16<E>(Buf + 8, Got);
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
  writeMipsHi16<E>(Buf, GotEntryAddr);
  writeMipsLo16<E>(Buf + 4, GotEntryAddr);
  writeMipsLo16<E>(Buf + 12, GotEntryAddr);
}

template <class ELFT>
void MipsTargetInfo<ELFT>::writeThunk(uint8_t *Buf, uint64_t S) const {
  // Write MIPS LA25 thunk code to call PIC function from the non-PIC one.
  // See MipsTargetInfo::writeThunk for details.
  const endianness E = ELFT::TargetEndianness;
  write32<E>(Buf, 0x3c190000);      // lui   $25, %hi(func)
  write32<E>(Buf + 4, 0x08000000);  // j     func
  write32<E>(Buf + 8, 0x27390000);  // addiu $25, $25, %lo(func)
  write32<E>(Buf + 12, 0x00000000); // nop
  writeMipsHi16<E>(Buf, S);
  write32<E>(Buf + 4, 0x08000000 | (S >> 2));
  writeMipsLo16<E>(Buf + 8, S);
}

template <class ELFT>
bool MipsTargetInfo<ELFT>::needsThunk(uint32_t Type, const InputFile &File,
                                      const SymbolBody &S) const {
  // Any MIPS PIC code function is invoked with its address in register $t9.
  // So if we have a branch instruction from non-PIC code to the PIC one
  // we cannot make the jump directly and need to create a small stubs
  // to save the target function address.
  // See page 3-38 ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
  if (Type != R_MIPS_26)
    return false;
  auto *F = dyn_cast<ELFFileBase<ELFT>>(&File);
  if (!F)
    return false;
  // If current file has PIC code, LA25 stub is not required.
  if (F->getObj().getHeader()->e_flags & EF_MIPS_PIC)
    return false;
  auto *D = dyn_cast<DefinedRegular<ELFT>>(&S);
  if (!D || !D->Section)
    return false;
  // LA25 is required if target file has PIC code
  // or target symbol is a PIC symbol.
  return (D->Section->getFile()->getObj().getHeader()->e_flags & EF_MIPS_PIC) ||
         (D->StOther & STO_MIPS_MIPS16) == STO_MIPS_PIC;
}

template <class ELFT>
uint64_t MipsTargetInfo<ELFT>::getImplicitAddend(const uint8_t *Buf,
                                                 uint32_t Type) const {
  const endianness E = ELFT::TargetEndianness;
  switch (Type) {
  default:
    return 0;
  case R_MIPS_32:
  case R_MIPS_GPREL32:
    return read32<E>(Buf);
  case R_MIPS_26:
    // FIXME (simon): If the relocation target symbol is not a PLT entry
    // we should use another expression for calculation:
    // ((A << 2) | (P & 0xf0000000)) >> 2
    return SignExtend64<28>((read32<E>(Buf) & 0x3ffffff) << 2);
  case R_MIPS_GPREL16:
  case R_MIPS_LO16:
  case R_MIPS_PCLO16:
  case R_MIPS_TLS_DTPREL_HI16:
  case R_MIPS_TLS_DTPREL_LO16:
  case R_MIPS_TLS_TPREL_HI16:
  case R_MIPS_TLS_TPREL_LO16:
    return readSignedLo16<E>(Buf);
  case R_MIPS_PC16:
    return getPcRelocAddend<E, 16, 2>(Buf);
  case R_MIPS_PC19_S2:
    return getPcRelocAddend<E, 19, 2>(Buf);
  case R_MIPS_PC21_S2:
    return getPcRelocAddend<E, 21, 2>(Buf);
  case R_MIPS_PC26_S2:
    return getPcRelocAddend<E, 26, 2>(Buf);
  case R_MIPS_PC32:
    return getPcRelocAddend<E, 32, 0>(Buf);
  }
}

static std::pair<uint32_t, uint64_t> calculateMips64RelChain(uint32_t Type,
                                                             uint64_t Val) {
  // MIPS N64 ABI packs multiple relocations into the single relocation
  // record. In general, all up to three relocations can have arbitrary
  // types. In fact, Clang and GCC uses only a few combinations. For now,
  // we support two of them. That is allow to pass at least all LLVM
  // test suite cases.
  // <any relocation> / R_MIPS_SUB / R_MIPS_HI16 | R_MIPS_LO16
  // <any relocation> / R_MIPS_64 / R_MIPS_NONE
  // The first relocation is a 'real' relocation which is calculated
  // using the corresponding symbol's value. The second and the third
  // relocations used to modify result of the first one: extend it to
  // 64-bit, extract high or low part etc. For details, see part 2.9 Relocation
  // at the https://dmz-portal.mips.com/mw/images/8/82/007-4658-001.pdf
  uint32_t Type2 = (Type >> 8) & 0xff;
  uint32_t Type3 = (Type >> 16) & 0xff;
  if (Type2 == R_MIPS_NONE && Type3 == R_MIPS_NONE)
    return std::make_pair(Type, Val);
  if (Type2 == R_MIPS_64 && Type3 == R_MIPS_NONE)
    return std::make_pair(Type2, Val);
  if (Type2 == R_MIPS_SUB && (Type3 == R_MIPS_HI16 || Type3 == R_MIPS_LO16))
    return std::make_pair(Type3, -Val);
  error("unsupported relocations combination " + Twine(Type));
  return std::make_pair(Type & 0xff, Val);
}

template <class ELFT>
void MipsTargetInfo<ELFT>::relocateOne(uint8_t *Loc, uint32_t Type,
                                       uint64_t Val) const {
  const endianness E = ELFT::TargetEndianness;
  // Thread pointer and DRP offsets from the start of TLS data area.
  // https://www.linux-mips.org/wiki/NPTL
  if (Type == R_MIPS_TLS_DTPREL_HI16 || Type == R_MIPS_TLS_DTPREL_LO16)
    Val -= 0x8000;
  else if (Type == R_MIPS_TLS_TPREL_HI16 || Type == R_MIPS_TLS_TPREL_LO16)
    Val -= 0x7000;
  if (ELFT::Is64Bits)
    std::tie(Type, Val) = calculateMips64RelChain(Type, Val);
  switch (Type) {
  case R_MIPS_32:
  case R_MIPS_GPREL32:
    write32<E>(Loc, Val);
    break;
  case R_MIPS_64:
    write64<E>(Loc, Val);
    break;
  case R_MIPS_26:
    write32<E>(Loc, (read32<E>(Loc) & ~0x3ffffff) | (Val >> 2));
    break;
  case R_MIPS_GOT_DISP:
  case R_MIPS_GOT_PAGE:
  case R_MIPS_GOT16:
  case R_MIPS_GPREL16:
    checkInt<16>(Val, Type);
  // fallthrough
  case R_MIPS_CALL16:
  case R_MIPS_GOT_OFST:
  case R_MIPS_LO16:
  case R_MIPS_PCLO16:
  case R_MIPS_TLS_DTPREL_LO16:
  case R_MIPS_TLS_TPREL_LO16:
    writeMipsLo16<E>(Loc, Val);
    break;
  case R_MIPS_HI16:
  case R_MIPS_PCHI16:
  case R_MIPS_TLS_DTPREL_HI16:
  case R_MIPS_TLS_TPREL_HI16:
    writeMipsHi16<E>(Loc, Val);
    break;
  case R_MIPS_JALR:
    // Ignore this optimization relocation for now
    break;
  case R_MIPS_PC16:
    applyMipsPcReloc<E, 16, 2>(Loc, Type, Val);
    break;
  case R_MIPS_PC19_S2:
    applyMipsPcReloc<E, 19, 2>(Loc, Type, Val);
    break;
  case R_MIPS_PC21_S2:
    applyMipsPcReloc<E, 21, 2>(Loc, Type, Val);
    break;
  case R_MIPS_PC26_S2:
    applyMipsPcReloc<E, 26, 2>(Loc, Type, Val);
    break;
  case R_MIPS_PC32:
    applyMipsPcReloc<E, 32, 0>(Loc, Type, Val);
    break;
  default:
    fatal("unrecognized reloc " + Twine(Type));
  }
}

template <class ELFT>
bool MipsTargetInfo<ELFT>::usesOnlyLowPageBits(uint32_t Type) const {
  return Type == R_MIPS_LO16 || Type == R_MIPS_GOT_OFST;
}
}
}
