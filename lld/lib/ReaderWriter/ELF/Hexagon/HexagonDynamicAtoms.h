//===- lib/ReaderWriter/ELF/Hexagon/HexagonDynamicAtoms.h -----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_HEXAGON_DYNAMIC_ATOMS_H
#define LLD_READER_WRITER_ELF_HEXAGON_DYNAMIC_ATOMS_H

#include "Atoms.h"
#include "HexagonTargetInfo.h"

/// \brief Specify various atom contents that are used by Hexagon dynamic
/// linking
namespace {
// .got atom
const uint8_t hexagonGotAtomContent[4] = { 0 };
// .got.plt atom (entry 0)
const uint8_t hexagonGotPlt0AtomContent[16] = { 0 };
// .got.plt atom (all other entries)
const uint8_t hexagonGotPltAtomContent[4] = { 0 };
// .plt (entry 0)
const uint8_t hexagonPlt0AtomContent[28] = {
  0x00, 0x40, 0x00, 0x00, // { immext (#0)
  0x1c, 0xc0, 0x49, 0x6a, //   r28 = add (pc, ##GOT0@PCREL) } # address of GOT0
  0x0e, 0x42, 0x9c, 0xe2, // { r14 -= add (r28, #16)  # offset of GOTn from GOTa
  0x4f, 0x40, 0x9c, 0x91, //   r15 = memw (r28 + #8)  # object ID at GOT2
  0x3c, 0xc0, 0x9c, 0x91, //   r28 = memw (r28 + #4) }# dynamic link at GOT1
  0x0e, 0x42, 0x0e, 0x8c, // { r14 = asr (r14, #2)    # index of PLTn
  0x00, 0xc0, 0x9c, 0x52, //   jumpr r28 }            # call dynamic linker
};

// .plt (other entries)
const uint8_t hexagonPltAtomContent[16] = {
  0x00, 0x40, 0x00, 0x00, // { immext (#0)
  0x0e, 0xc0, 0x49, 0x6a, //   r14 = add (pc, ##GOTn@PCREL) } # address of GOTn
  0x1c, 0xc0, 0x8e, 0x91, // r28 = memw (r14)                 # contents of GOTn
  0x00, 0xc0, 0x9c, 0x52, // jumpr r28                        # call it
};
}

namespace lld {
namespace elf {

class HexagonGOTAtom : public GOTAtom {
public:
  HexagonGOTAtom(const File &f) : GOTAtom(f, ".got") {}

  virtual ArrayRef<uint8_t> rawContent() const {
    return ArrayRef<uint8_t>(hexagonGotAtomContent, 4);
  }

  virtual Alignment alignment() const { return Alignment(2); }
};

class HexagonGOTPLTAtom : public GOTAtom {
public:
  HexagonGOTPLTAtom(const File &f) : GOTAtom(f, ".got.plt") {}

  virtual ArrayRef<uint8_t> rawContent() const {
    return ArrayRef<uint8_t>(hexagonGotPltAtomContent, 4);
  }

  virtual Alignment alignment() const { return Alignment(2); }
};

class HexagonGOTPLT0Atom : public GOTAtom {
public:
  HexagonGOTPLT0Atom(const File &f) : GOTAtom(f, ".got.plt") {}

  virtual ArrayRef<uint8_t> rawContent() const {
    return ArrayRef<uint8_t>(hexagonGotPlt0AtomContent, 16);
  }

  virtual Alignment alignment() const { return Alignment(3); }
};

class HexagonPLT0Atom : public PLT0Atom {
public:
  HexagonPLT0Atom(const File &f) : PLT0Atom(f) {
#ifndef NDEBUG
    _name = ".PLT0";
#endif
  }

  virtual ArrayRef<uint8_t> rawContent() const {
    return ArrayRef<uint8_t>(hexagonPlt0AtomContent, 28);
  }
};

class HexagonPLTAtom : public PLTAtom {

public:
  HexagonPLTAtom(const File &f, StringRef secName) : PLTAtom(f, secName) {}

  virtual ArrayRef<uint8_t> rawContent() const {
    return ArrayRef<uint8_t>(hexagonPltAtomContent, 16);
  }
};

} // elf
} // lld

#endif // LLD_READER_WRITER_ELF_HEXAGON_DYNAMIC_ATOMS_H
