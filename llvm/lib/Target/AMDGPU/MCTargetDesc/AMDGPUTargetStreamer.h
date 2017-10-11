//===-- AMDGPUTargetStreamer.h - AMDGPU Target Streamer --------*- C++ -*--===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_MCTARGETDESC_AMDGPUTARGETSTREAMER_H
#define LLVM_LIB_TARGET_AMDGPU_MCTARGETDESC_AMDGPUTARGETSTREAMER_H

#include "AMDGPUHSAMetadataStreamer.h"
#include "AMDKernelCodeT.h"
#include "llvm/MC/MCStreamer.h"

namespace llvm {
#include "AMDGPUPTNote.h"

class DataLayout;
class Function;
class MCELFStreamer;
class MCSymbol;
class MDNode;
class Module;
class Type;

class AMDGPUTargetStreamer : public MCTargetStreamer {
protected:
  AMDGPU::HSAMD::MetadataStreamer HSAMetadataStreamer;
  MCContext &getContext() const { return Streamer.getContext(); }

public:
  AMDGPUTargetStreamer(MCStreamer &S);
  virtual void EmitDirectiveHSACodeObjectVersion(uint32_t Major,
                                                 uint32_t Minor) = 0;

  virtual void EmitDirectiveHSACodeObjectISA(uint32_t Major, uint32_t Minor,
                                             uint32_t Stepping,
                                             StringRef VendorName,
                                             StringRef ArchName) = 0;

  virtual void EmitAMDKernelCodeT(const amd_kernel_code_t &Header) = 0;

  virtual void EmitAMDGPUSymbolType(StringRef SymbolName, unsigned Type) = 0;

  virtual void EmitStartOfHSAMetadata(const Module &Mod);

  virtual void EmitKernelHSAMetadata(
      const Function &Func, const amd_kernel_code_t &KernelCode);

  virtual void EmitEndOfHSAMetadata();

  /// \returns True on success, false on failure.
  virtual bool EmitHSAMetadata(StringRef YamlString) = 0;

  virtual bool EmitPalMetadata(ArrayRef<uint32_t> Data) = 0;
};

class AMDGPUTargetAsmStreamer final : public AMDGPUTargetStreamer {
  formatted_raw_ostream &OS;
public:
  AMDGPUTargetAsmStreamer(MCStreamer &S, formatted_raw_ostream &OS);
  void EmitDirectiveHSACodeObjectVersion(uint32_t Major,
                                         uint32_t Minor) override;

  void EmitDirectiveHSACodeObjectISA(uint32_t Major, uint32_t Minor,
                                     uint32_t Stepping, StringRef VendorName,
                                     StringRef ArchName) override;

  void EmitAMDKernelCodeT(const amd_kernel_code_t &Header) override;

  void EmitAMDGPUSymbolType(StringRef SymbolName, unsigned Type) override;

  /// \returns True on success, false on failure.
  bool EmitHSAMetadata(StringRef YamlString) override;

  bool EmitPalMetadata(ArrayRef<uint32_t> data) override;
};

class AMDGPUTargetELFStreamer final : public AMDGPUTargetStreamer {
  MCStreamer &Streamer;

  void EmitAMDGPUNote(const MCExpr *DescSize,
                      AMDGPU::ElfNote::NoteType Type,
                      function_ref<void(MCELFStreamer &)> EmitDesc);

public:
  AMDGPUTargetELFStreamer(MCStreamer &S);

  MCELFStreamer &getStreamer();

  void EmitDirectiveHSACodeObjectVersion(uint32_t Major,
                                         uint32_t Minor) override;

  void EmitDirectiveHSACodeObjectISA(uint32_t Major, uint32_t Minor,
                                     uint32_t Stepping, StringRef VendorName,
                                     StringRef ArchName) override;

  void EmitAMDKernelCodeT(const amd_kernel_code_t &Header) override;

  void EmitAMDGPUSymbolType(StringRef SymbolName, unsigned Type) override;

  /// \returns True on success, false on failure.
  bool EmitHSAMetadata(StringRef YamlString) override;

  bool EmitPalMetadata(ArrayRef<uint32_t> data) override;
};

}
#endif
