//===- MCWin64EH.h - Machine Code Win64 EH support --------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains declarations to support the Win64 Exception Handling
// scheme in MC.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCWIN64EH_H
#define LLVM_MC_MCWIN64EH_H

#include "llvm/Support/Win64EH.h"
#include <cassert>
#include <vector>

namespace llvm {
  class StringRef;
  class MCStreamer;
  class MCSymbol;

struct MCWin64EHInstruction {
  typedef Win64EH::UnwindOpcodes OpType;
  const OpType Operation;
  const MCSymbol *Label;
  const unsigned Offset;
  const unsigned Register;

  MCWin64EHInstruction(OpType Op, MCSymbol *L, unsigned Reg)
      : Operation(Op), Label(L), Offset(0), Register(Reg) {
    assert(Op == Win64EH::UOP_PushNonVol);
  }

  MCWin64EHInstruction(MCSymbol *L, unsigned Size)
      : Operation(Size > 128 ? Win64EH::UOP_AllocLarge
                             : Win64EH::UOP_AllocSmall),
        Label(L), Offset(Size), Register(-1) {}

  MCWin64EHInstruction(OpType Op, MCSymbol *L, unsigned Reg, unsigned Off)
      : Operation(Op), Label(L), Offset(Off), Register(Reg) {
    assert(Op == Win64EH::UOP_SetFPReg ||
           Op == Win64EH::UOP_SaveNonVol ||
           Op == Win64EH::UOP_SaveNonVolBig ||
           Op == Win64EH::UOP_SaveXMM128 ||
           Op == Win64EH::UOP_SaveXMM128Big);
  }

  MCWin64EHInstruction(OpType Op, MCSymbol *L, bool Code)
      : Operation(Op), Label(L), Offset(Code ? 1 : 0), Register(-1) {
    assert(Op == Win64EH::UOP_PushMachFrame);
  }
};

  struct MCWinFrameInfo {
    MCWinFrameInfo()
      : Begin(nullptr), End(nullptr),ExceptionHandler(nullptr),
        Function(nullptr), PrologEnd(nullptr), Symbol(nullptr),
        HandlesUnwind(false), HandlesExceptions(false), LastFrameInst(-1),
        ChainedParent(nullptr), Instructions() {}
    MCSymbol *Begin;
    MCSymbol *End;
    const MCSymbol *ExceptionHandler;
    const MCSymbol *Function;
    MCSymbol *PrologEnd;
    MCSymbol *Symbol;
    bool HandlesUnwind;
    bool HandlesExceptions;
    int LastFrameInst;
    MCWinFrameInfo *ChainedParent;
    std::vector<MCWin64EHInstruction> Instructions;
  };

  class MCWin64EHUnwindEmitter {
  public:
    static StringRef GetSectionSuffix(const MCSymbol *func);
    //
    // This emits the unwind info sections (.pdata and .xdata in PE/COFF).
    //
    static void Emit(MCStreamer &streamer);
    static void EmitUnwindInfo(MCStreamer &streamer, MCWinFrameInfo *info);
  };
} // end namespace llvm

#endif
