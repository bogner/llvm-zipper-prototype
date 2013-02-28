//===- lib/ReaderWriter/ELF/Hexagon/HexagonRelocationHandler.cpp ---------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "HexagonTargetHandler.h"
#include "HexagonTargetInfo.h"
#include "HexagonRelocationHandler.h"
#include "HexagonRelocationFunctions.h"


using namespace lld;
using namespace elf;

using namespace llvm::ELF;

namespace {
int relocBNPCREL(uint8_t *location, uint64_t P, uint64_t S, uint64_t A,
                 int32_t nBits) {
  int32_t result = (uint32_t)(((S + A) - P) >> 2);
  int32_t range = 1 << nBits;
  if (result < range && result > -range) {
    result = lld::scatterBits<int32_t>(result, FINDV4BITMASK(location));
    *reinterpret_cast<llvm::support::ulittle32_t *>(location) =
        result |
        (uint32_t) * reinterpret_cast<llvm::support::ulittle32_t *>(location);
    return 0;
  }
  return 1;
}

/// \brief Word32_LO: 0x00c03fff : (S + A) : Truncate
int relocLO16(uint8_t *location, uint64_t P, uint64_t S, uint64_t A) {
  uint32_t result = (uint32_t)(S + A);
  result = lld::scatterBits<int32_t>(result, 0x00c03fff);
  *reinterpret_cast<llvm::support::ulittle32_t *>(location) = result |
            (uint32_t)*reinterpret_cast<llvm::support::ulittle32_t *>(location);
  return 0;
}

/// \brief Word32_LO: 0x00c03fff : (S + A) >> 16 : Truncate
int relocHI16(uint8_t *location, uint64_t P, uint64_t S, uint64_t A) {
  uint32_t result = (uint32_t)((S + A)>>16);
  result = lld::scatterBits<int32_t>(result, 0x00c03fff);
  *reinterpret_cast<llvm::support::ulittle32_t *>(location) = result |
            (uint32_t)*reinterpret_cast<llvm::support::ulittle32_t *>(location);
  return 0;
}

/// \brief Word32: 0xffffffff : (S + A) : Truncate
int reloc32(uint8_t *location, uint64_t P, uint64_t S, uint64_t A) {
  uint32_t result = (uint32_t)(S + A);
  *reinterpret_cast<llvm::support::ulittle32_t *>(location) =
      result |
      (uint32_t) * reinterpret_cast<llvm::support::ulittle32_t *>(location);
  return 0;
}

int reloc32_6_X(uint8_t *location, uint64_t P, uint64_t S, uint64_t A) {
  int64_t result = ((S + A) >> 6);
  int64_t range = 1L << 32;
  if (result > range)
    return 1;
  result = lld::scatterBits<int32_t>(result, 0xfff3fff);
  *reinterpret_cast<llvm::support::ulittle32_t *>(location) =
      result |
      (uint32_t) * reinterpret_cast<llvm::support::ulittle32_t *>(location);
  return 0;
}

// R_HEX_B32_PCREL_X
int relocHexB32PCRELX(uint8_t *location, uint64_t P, uint64_t S, uint64_t A) {
  int64_t result = ((S + A - P) >> 6);
  result = lld::scatterBits<int32_t>(result, 0xfff3fff);
  *reinterpret_cast<llvm::support::ulittle32_t *>(location) =
      result |
      (uint32_t) * reinterpret_cast<llvm::support::ulittle32_t *>(location);
  return 0;
}

// R_HEX_BN_PCREL_X
int relocHexBNPCRELX(uint8_t *location, uint64_t P, uint64_t S, uint64_t A,
                     int nbits) {
  int32_t result = ((S + A - P) & 0x3f);
  int32_t range = 1 << nbits;
  if (result < range && result > -range) {
    result = lld::scatterBits<int32_t>(result, FINDV4BITMASK(location));
    *reinterpret_cast<llvm::support::ulittle32_t *>(location) =
        result |
        (uint32_t) * reinterpret_cast<llvm::support::ulittle32_t *>(location);
    return 0;
  }
  return 1;
}

// R_HEX_N_X : Word32_U6 : (S + A) : Unsigned Truncate
int relocHex_N_X(uint8_t *location, uint64_t P, uint64_t S, uint64_t A) {
  uint32_t result = (S + A);
  result = lld::scatterBits<uint32_t>(result, FINDV4BITMASK(location));
  *reinterpret_cast<llvm::support::ulittle32_t *>(location) =
      result |
      (uint32_t) * reinterpret_cast<llvm::support::ulittle32_t *>(location);
  return 0;
}

} // end anon namespace

ErrorOr<void> HexagonTargetRelocationHandler::applyRelocation(
    ELFWriter &writer, llvm::FileOutputBuffer &buf, const AtomLayout &atom,
    const Reference &ref) const {
  uint8_t *atomContent = buf.getBufferStart() + atom._fileOffset;
  uint8_t *location = atomContent + ref.offsetInAtom();
  uint64_t targetVAddress = writer.addressOfAtom(ref.target());
  uint64_t relocVAddress = atom._virtualAddr + ref.offsetInAtom();

  switch (ref.kind()) {
  case R_HEX_B22_PCREL:
    relocBNPCREL(location, relocVAddress, targetVAddress, ref.addend(), 21);
    break;
  case R_HEX_B15_PCREL:
    relocBNPCREL(location, relocVAddress, targetVAddress, ref.addend(), 14);
    break;
  case R_HEX_B9_PCREL:
    relocBNPCREL(location, relocVAddress, targetVAddress, ref.addend(), 8);
    break;
  case R_HEX_LO16:
    relocLO16(location, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_HEX_HI16:
    relocHI16(location, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_HEX_32:
    reloc32(location, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_HEX_32_6_X:
    reloc32_6_X(location, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_HEX_B32_PCREL_X:
    relocHexB32PCRELX(location, relocVAddress, targetVAddress, ref.addend());
    break;
  case R_HEX_B22_PCREL_X:
    relocHexBNPCRELX(location, relocVAddress, targetVAddress, ref.addend(), 21);
    break;
  case R_HEX_B15_PCREL_X:
    relocHexBNPCRELX(location, relocVAddress, targetVAddress, ref.addend(), 14);
    break;
  case R_HEX_B13_PCREL_X:
    relocHexBNPCRELX(location, relocVAddress, targetVAddress, ref.addend(), 12);
    break;
  case R_HEX_B9_PCREL_X:
    relocHexBNPCRELX(location, relocVAddress, targetVAddress, ref.addend(), 8);
    break;
  case R_HEX_B7_PCREL_X:
    relocHexBNPCRELX(location, relocVAddress, targetVAddress, ref.addend(), 6);
    break;
  case R_HEX_16_X:
  case R_HEX_12_X:
  case R_HEX_11_X:
  case R_HEX_10_X:
  case R_HEX_9_X:
  case R_HEX_8_X:
  case R_HEX_7_X:
  case R_HEX_6_X:
    relocHex_N_X(location, relocVAddress, targetVAddress, ref.addend());
    break;
  case lld::Reference::kindLayoutAfter:
  case lld::Reference::kindLayoutBefore:
  case lld::Reference::kindInGroup:
    break;
  default : {
    std::string str;
    llvm::raw_string_ostream s(str);
    auto name = _targetInfo.stringFromRelocKind(ref.kind());
    s << "Unhandled relocation: "
      << (name ? *name : "<unknown>" ) << " (" << ref.kind() << ")";
    s.flush();
    llvm_unreachable(str.c_str());
  }
  }

  return error_code::success();
}
