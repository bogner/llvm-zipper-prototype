//===- lib/ReaderWriter/ELF/Mips/MipsRelocationHandler.cpp ----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "MipsTargetHandler.h"
#include "MipsLinkingContext.h"
#include "MipsRelocationHandler.h"
#include "lld/ReaderWriter/RelocationHelperFunctions.h"

using namespace lld;
using namespace elf;
using namespace llvm::ELF;
using namespace llvm::support;

namespace {
enum class CrossJumpMode {
  None,      // Not a jump or non-isa-cross jump
  ToRegular, // cross isa jump to regular symbol
  ToMicro    // cross isa jump to microMips symbol
};
}

static inline void applyReloc(uint32_t &ins, uint32_t result, uint32_t mask) {
  ins = (ins & ~mask) | (result & mask);
}

template <size_t BITS, class T> inline T signExtend(T val) {
  if (val & (T(1) << (BITS - 1)))
    val |= T(-1) << BITS;
  return val;
}

/// \brief R_MIPS_32
/// local/external: word32 S + A (truncate)
static void reloc32(uint32_t &ins, uint64_t S, int64_t A) {
  applyReloc(ins, S + A, 0xffffffff);
}

/// \brief R_MIPS_PC32
/// local/external: word32 S + A i- P (truncate)
void relocpc32(uint32_t &ins, uint64_t P, uint64_t S, int64_t A) {
  applyReloc(ins, S + A - P, 0xffffffff);
}

/// \brief R_MIPS_26, R_MICROMIPS_26_S1
/// local   : ((A | ((P + 4) & 0x3F000000)) + S) >> 2
static void reloc26loc(uint32_t &ins, uint64_t P, uint64_t S, int32_t A,
                       uint32_t shift) {
  uint32_t result = (A | ((P + 4) & (0xfc000000 << shift))) + S;
  applyReloc(ins, result >> shift, 0x03ffffff);
}

/// \brief LLD_R_MIPS_GLOBAL_26, LLD_R_MICROMIPS_GLOBAL_26_S1
/// external: (sign-extend(A) + S) >> 2
static void reloc26ext(uint32_t &ins, uint64_t S, int32_t A, uint32_t shift) {
  uint32_t result = shift == 1 ? signExtend<27>(A) : signExtend<28>(A);
  result += S;
  applyReloc(ins, result >> shift, 0x03ffffff);
}

/// \brief R_MIPS_HI16, R_MIPS_TLS_DTPREL_HI16, R_MIPS_TLS_TPREL_HI16,
/// R_MICROMIPS_HI16, R_MICROMIPS_TLS_DTPREL_HI16, R_MICROMIPS_TLS_TPREL_HI16,
/// LLD_R_MIPS_HI16
/// local/external: hi16 (AHL + S) - (short)(AHL + S) (truncate)
/// _gp_disp      : hi16 (AHL + GP - P) - (short)(AHL + GP - P) (verify)
static void relocHi16(uint32_t &ins, uint64_t P, uint64_t S, int64_t AHL,
                      bool isGPDisp) {
  int32_t result = isGPDisp ? AHL + S - P : AHL + S;
  applyReloc(ins, (result + 0x8000) >> 16, 0xffff);
}

/// \brief R_MIPS_LO16, R_MIPS_TLS_DTPREL_LO16, R_MIPS_TLS_TPREL_LO16,
/// R_MICROMIPS_LO16, R_MICROMIPS_TLS_DTPREL_LO16, R_MICROMIPS_TLS_TPREL_LO16,
/// LLD_R_MIPS_LO16
/// local/external: lo16 AHL + S (truncate)
/// _gp_disp      : lo16 AHL + GP - P + 4 (verify)
static void relocLo16(uint32_t &ins, uint64_t P, uint64_t S, int64_t AHL,
                      bool isGPDisp, bool micro) {
  int32_t result = isGPDisp ? AHL + S - P + (micro ? 3 : 4) : AHL + S;
  applyReloc(ins, result, 0xffff);
}

/// \brief R_MIPS_GOT16, R_MIPS_CALL16, R_MICROMIPS_GOT16, R_MICROMIPS_CALL16
/// rel16 G (verify)
static void relocGOT(uint32_t &ins, uint64_t S, uint64_t GP) {
  int32_t G = (int32_t)(S - GP);
  applyReloc(ins, G, 0xffff);
}

/// \brief R_MIPS_GPREL16
/// local: sign-extend(A) + S + GP0 - GP
/// external: sign-extend(A) + S - GP
static void relocGPRel16(uint32_t &ins, uint64_t S, int64_t A, uint64_t GP) {
  // We added GP0 to addendum for a local symbol during a Relocation pass.
  int32_t result = signExtend<16>(A) + S - GP;
  applyReloc(ins, result, 0xffff);
}

/// \brief R_MIPS_GPREL32
/// local: rel32 A + S + GP0 - GP (truncate)
static void relocGPRel32(uint32_t &ins, uint64_t S, int64_t A, uint64_t GP) {
  // We added GP0 to addendum for a local symbol during a Relocation pass.
  int32_t result = A + S - GP;
  applyReloc(ins, result, 0xffffffff);
}

/// \brief R_MICROMIPS_PC7_S1
static void relocPc7(uint32_t &ins, uint64_t P, uint64_t S, int64_t A) {
  A = signExtend<8>(A);
  int32_t result = S + A - P;
  applyReloc(ins, result >> 1, 0x7f);
}

/// \brief R_MICROMIPS_PC10_S1
static void relocPc10(uint32_t &ins, uint64_t P, uint64_t S, int64_t A) {
  A = signExtend<11>(A);
  int32_t result = S + A - P;
  applyReloc(ins, result >> 1, 0x3ff);
}

/// \brief R_MICROMIPS_PC16_S1
static void relocPc16(uint32_t &ins, uint64_t P, uint64_t S, int64_t A) {
  A = signExtend<17>(A);
  int32_t result = S + A - P;
  applyReloc(ins, result >> 1, 0xffff);
}

/// \brief R_MICROMIPS_PC23_S2
static void relocPc23(uint32_t &ins, uint64_t P, uint64_t S, int64_t A) {
  A = signExtend<25>(A);
  int32_t result = S + A - P;

  // Check addiupc 16MB range.
  if (result + 0x1000000 >= 0x2000000)
    llvm::errs() << "The addiupc instruction immediate "
                 << llvm::format_hex(result, 10) << " is out of range.\n";
  else
    applyReloc(ins, result >> 2, 0x7fffff);
}

/// \brief LLD_R_MIPS_32_HI16
static void reloc32hi16(uint32_t &ins, uint64_t S, int64_t A) {
  applyReloc(ins, (S + A + 0x8000) & 0xffff0000, 0xffffffff);
}

static std::error_code adjustJumpOpCode(uint32_t &ins, uint64_t tgt,
                                        CrossJumpMode mode) {
  if (mode == CrossJumpMode::None)
    return std::error_code();

  bool toMicro = mode == CrossJumpMode::ToMicro;
  uint32_t opNative = toMicro ? 0x03 : 0x3d;
  uint32_t opCross = toMicro ? 0x1d : 0x3c;

  if ((tgt & 1) != toMicro)
    return make_dynamic_error_code(
        Twine("Incorrect bit 0 for the jalx target"));

  if (tgt & 2)
    return make_dynamic_error_code(Twine("The jalx target 0x") +
                                   Twine::utohexstr(tgt) +
                                   " is not word-aligned");
  uint8_t op = ins >> 26;
  if (op != opNative && op != opCross)
    return make_dynamic_error_code(Twine("Unsupported jump opcode (0x") +
                                   Twine::utohexstr(op) +
                                   ") for ISA modes cross call");

  ins = (ins & ~(0x3f << 26)) | (opCross << 26);
  return std::error_code();
}

static bool isMicroMipsAtom(const Atom *a) {
  if (const auto *da = dyn_cast<DefinedAtom>(a))
    return da->codeModel() == DefinedAtom::codeMipsMicro ||
           da->codeModel() == DefinedAtom::codeMipsMicroPIC;
  return false;
}

static CrossJumpMode getCrossJumpMode(const Reference &ref) {
  if (!isa<DefinedAtom>(ref.target()))
    return CrossJumpMode::None;
  bool isTgtMicro = isMicroMipsAtom(ref.target());
  switch (ref.kindValue()) {
  case R_MIPS_26:
  case LLD_R_MIPS_GLOBAL_26:
    return isTgtMicro ? CrossJumpMode::ToMicro : CrossJumpMode::None;
  case R_MICROMIPS_26_S1:
  case LLD_R_MICROMIPS_GLOBAL_26_S1:
    return isTgtMicro ? CrossJumpMode::None : CrossJumpMode::ToRegular;
  default:
    return CrossJumpMode::None;
  }
}

static bool needMicroShuffle(const Reference &ref) {
  if (ref.kindNamespace() != lld::Reference::KindNamespace::ELF)
    return false;
  assert(ref.kindArch() == Reference::KindArch::Mips);
  switch (ref.kindValue()) {
  case R_MICROMIPS_HI16:
  case R_MICROMIPS_LO16:
  case R_MICROMIPS_GOT16:
  case R_MICROMIPS_PC16_S1:
  case R_MICROMIPS_PC23_S2:
  case R_MICROMIPS_CALL16:
  case R_MICROMIPS_26_S1:
  case R_MICROMIPS_TLS_GD:
  case R_MICROMIPS_TLS_LDM:
  case R_MICROMIPS_TLS_DTPREL_HI16:
  case R_MICROMIPS_TLS_DTPREL_LO16:
  case R_MICROMIPS_TLS_GOTTPREL:
  case R_MICROMIPS_TLS_TPREL_HI16:
  case R_MICROMIPS_TLS_TPREL_LO16:
  case LLD_R_MICROMIPS_GLOBAL_26_S1:
    return true;
  default:
    return false;
  }
}

static uint32_t microShuffle(uint32_t ins) {
  return ((ins & 0xffff) << 16) | ((ins & 0xffff0000) >> 16);
}

namespace {

template <class ELFT> class RelocationHandler : public TargetRelocationHandler {
public:
  RelocationHandler(MipsLinkingContext &ctx) : _ctx(ctx) {}

  std::error_code applyRelocation(ELFWriter &writer,
                                  llvm::FileOutputBuffer &buf,
                                  const lld::AtomLayout &atom,
                                  const Reference &ref) const override;

private:
  MipsLinkingContext &_ctx;

  MipsTargetLayout<ELFT> &getTargetLayout() const {
    return static_cast<MipsTargetLayout<ELFT> &>(
        _ctx.getTargetHandler<ELFT>().getTargetLayout());
  }
};

template <class ELFT>
std::error_code RelocationHandler<ELFT>::applyRelocation(
    ELFWriter &writer, llvm::FileOutputBuffer &buf, const lld::AtomLayout &atom,
    const Reference &ref) const {
  if (ref.kindNamespace() != lld::Reference::KindNamespace::ELF)
    return std::error_code();
  assert(ref.kindArch() == Reference::KindArch::Mips);

  AtomLayout *gpAtom = getTargetLayout().getGP();
  uint64_t gpAddr = gpAtom ? gpAtom->_virtualAddr : 0;

  AtomLayout *gpDispAtom = getTargetLayout().getGPDisp();
  bool isGpDisp = gpDispAtom && ref.target() == gpDispAtom->_atom;

  uint8_t *atomContent = buf.getBufferStart() + atom._fileOffset;
  uint8_t *location = atomContent + ref.offsetInAtom();
  uint64_t targetVAddress = writer.addressOfAtom(ref.target());
  uint64_t relocVAddress = atom._virtualAddr + ref.offsetInAtom();
  uint32_t ins = endian::read<uint32_t, little, 2>(location);

  bool shuffle = needMicroShuffle(ref);
  if (shuffle)
    ins = microShuffle(ins);

  if (isMicroMipsAtom(ref.target()))
    targetVAddress |= 1;

  CrossJumpMode crossJump = getCrossJumpMode(ref);
  if (auto ec = adjustJumpOpCode(ins, targetVAddress, crossJump))
    return ec;

  switch (ref.kindValue()) {
  case R_MIPS_NONE:
    break;
  case R_MIPS_32:
    reloc32(ins, targetVAddress, ref.addend());
    break;
  case R_MIPS_26:
    reloc26loc(ins, relocVAddress, targetVAddress, ref.addend(), 2);
    break;
  case R_MICROMIPS_26_S1:
    reloc26loc(ins, relocVAddress, targetVAddress, ref.addend(),
               crossJump != CrossJumpMode::None ? 2 : 1);
    break;
  case R_MIPS_HI16:
    relocHi16(ins, relocVAddress, targetVAddress, ref.addend(), isGpDisp);
    break;
  case R_MICROMIPS_HI16:
    relocHi16(ins, relocVAddress, targetVAddress, ref.addend(), isGpDisp);
    break;
  case R_MIPS_LO16:
    relocLo16(ins, relocVAddress, targetVAddress, ref.addend(), isGpDisp,
              false);
    break;
  case R_MICROMIPS_LO16:
    relocLo16(ins, relocVAddress, targetVAddress, ref.addend(), isGpDisp, true);
    break;
  case R_MIPS_GOT16:
  case R_MIPS_CALL16:
    relocGOT(ins, targetVAddress, gpAddr);
    break;
  case R_MICROMIPS_GOT16:
  case R_MICROMIPS_CALL16:
    relocGOT(ins, targetVAddress, gpAddr);
    break;
  case R_MICROMIPS_PC7_S1:
    relocPc7(ins, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_MICROMIPS_PC10_S1:
    relocPc10(ins, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_MICROMIPS_PC16_S1:
    relocPc16(ins, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_MICROMIPS_PC23_S2:
    relocPc23(ins, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_MIPS_TLS_GD:
  case R_MIPS_TLS_LDM:
  case R_MIPS_TLS_GOTTPREL:
  case R_MICROMIPS_TLS_GD:
  case R_MICROMIPS_TLS_LDM:
  case R_MICROMIPS_TLS_GOTTPREL:
    relocGOT(ins, targetVAddress, gpAddr);
    break;
  case R_MIPS_TLS_DTPREL_HI16:
  case R_MIPS_TLS_TPREL_HI16:
  case R_MICROMIPS_TLS_DTPREL_HI16:
  case R_MICROMIPS_TLS_TPREL_HI16:
    relocHi16(ins, 0, targetVAddress, ref.addend(), false);
    break;
  case R_MIPS_TLS_DTPREL_LO16:
  case R_MIPS_TLS_TPREL_LO16:
    relocLo16(ins, 0, targetVAddress, ref.addend(), false, false);
    break;
  case R_MICROMIPS_TLS_DTPREL_LO16:
  case R_MICROMIPS_TLS_TPREL_LO16:
    relocLo16(ins, 0, targetVAddress, ref.addend(), false, true);
    break;
  case R_MIPS_GPREL16:
    relocGPRel16(ins, targetVAddress, ref.addend(), gpAddr);
    break;
  case R_MIPS_GPREL32:
    relocGPRel32(ins, targetVAddress, ref.addend(), gpAddr);
    break;
  case R_MIPS_JALR:
  case R_MICROMIPS_JALR:
    // We do not do JALR optimization now.
    break;
  case R_MIPS_REL32:
  case R_MIPS_JUMP_SLOT:
  case R_MIPS_COPY:
  case R_MIPS_TLS_DTPMOD32:
  case R_MIPS_TLS_DTPREL32:
  case R_MIPS_TLS_TPREL32:
    // Ignore runtime relocations.
    break;
  case R_MIPS_PC32:
    relocpc32(ins, relocVAddress, targetVAddress, ref.addend());
    break;
  case LLD_R_MIPS_GLOBAL_GOT:
    // Do nothing.
    break;
  case LLD_R_MIPS_32_HI16:
    reloc32hi16(ins, targetVAddress, ref.addend());
    break;
  case LLD_R_MIPS_GLOBAL_26:
    reloc26ext(ins, targetVAddress, ref.addend(), 2);
    break;
  case LLD_R_MICROMIPS_GLOBAL_26_S1:
    reloc26ext(ins, targetVAddress, ref.addend(),
               crossJump != CrossJumpMode::None ? 2 : 1);
    break;
  case LLD_R_MIPS_HI16:
    relocHi16(ins, 0, targetVAddress, 0, false);
    break;
  case LLD_R_MIPS_LO16:
    relocLo16(ins, 0, targetVAddress, 0, false, false);
    break;
  case LLD_R_MIPS_STO_PLT:
    // Do nothing.
    break;
  default:
    return make_unhandled_reloc_error();
  }

  if (shuffle)
    ins = microShuffle(ins);

  endian::write<uint32_t, little, 2>(location, ins);
  return std::error_code();
}

} // end anon namespace

namespace lld {
namespace elf {

template <>
std::unique_ptr<TargetRelocationHandler>
createMipsRelocationHandler<Mips32ELType>(MipsLinkingContext &ctx) {
  return std::unique_ptr<TargetRelocationHandler>(
      new RelocationHandler<Mips32ELType>(ctx));
}

template <>
std::unique_ptr<TargetRelocationHandler>
createMipsRelocationHandler<Mips64ELType>(MipsLinkingContext &ctx) {
  return std::unique_ptr<TargetRelocationHandler>(
      new RelocationHandler<Mips64ELType>(ctx));
}

} // elf
} // lld
