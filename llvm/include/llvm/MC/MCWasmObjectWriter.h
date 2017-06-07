//===-- llvm/MC/MCWasmObjectWriter.h - Wasm Object Writer -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCWASMOBJECTWRITER_H
#define LLVM_MC_MCWASMOBJECTWRITER_H

#include "llvm/ADT/Triple.h"
#include "llvm/BinaryFormat/Wasm.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

namespace llvm {
class MCAssembler;
class MCContext;
class MCFixup;
class MCFragment;
class MCObjectWriter;
class MCSectionWasm;
class MCSymbol;
class MCSymbolWasm;
class MCValue;
class raw_pwrite_stream;

class MCWasmObjectTargetWriter {
  const unsigned Is64Bit : 1;

protected:
  explicit MCWasmObjectTargetWriter(bool Is64Bit_);

public:
  virtual ~MCWasmObjectTargetWriter();

  virtual unsigned getRelocType(MCContext &Ctx, const MCValue &Target,
                                const MCFixup &Fixup, bool IsPCRel) const = 0;

  /// \name Accessors
  /// @{
  bool is64Bit() const { return Is64Bit; }
  /// @}
};

/// \brief Construct a new Wasm writer instance.
///
/// \param MOTW - The target specific Wasm writer subclass.
/// \param OS - The stream to write to.
/// \returns The constructed object writer.
MCObjectWriter *createWasmObjectWriter(MCWasmObjectTargetWriter *MOTW,
                                       raw_pwrite_stream &OS);
} // End llvm namespace

#endif
