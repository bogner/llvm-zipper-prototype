//==-- llvm/CodeGen/GlobalISel/RegisterBank.h - Register Bank ----*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file This file declares the API of register banks.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GLOBALISEL_REGBANK_H
#define LLVM_CODEGEN_GLOBALISEL_REGBANK_H

#include "llvm/ADT/BitVector.h"

namespace llvm {
// Forward declarations.
class RegisterBankInfo;
class TargetRegisterClass;
class TargetRegisterInfo;

/// This class implements the register bank concept.
/// Two instances of RegisterBank must have different ID.
/// This property is enforced by the RegisterBankInfo class.
class RegisterBank {
private:
  unsigned ID;
  const char *Name;
  unsigned Size;
  BitVector ContainedRegClass;

  /// Only the RegisterBankInfo can create RegisterBank.
  RegisterBank() = default;

  friend RegisterBankInfo;

public:
  /// Get the identifier of this register bank.
  unsigned getID() const { return ID; }

  /// Get a user friendly name of this register bank.
  /// Should be used only for debugging purposes.
  const char *getName() const { return Name; }

  /// Get the maximal size in bits that fits in this register bank.
  unsigned getSize() const { return Size; }

  /// Check if this register bank is valid. In other words,
  /// if it has been properly constructed.
  void verify(const TargetRegisterInfo &TRI) const;

  /// Check whether this register bank contains \p RC.
  /// In other words, check if this register bank fully covers
  /// the registers that \p RC contains.
  bool contains(const TargetRegisterClass &RC) const;

  /// Check whether \p OtherRB is the same as this.
  bool operator==(const RegisterBank &OtherRB) const;
  bool operator!=(const RegisterBank &OtherRB) const {
    return !this->operator==(OtherRB);
  }
};
} // End namespace llvm.

#endif
