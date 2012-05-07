//===------ polly/RegisterPasses.h - Register the Polly passes *- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Functions to register the Polly passes in a LLVM pass manager.
//
//===----------------------------------------------------------------------===//

#ifndef POLLY_REGISTER_PASSES_H
#define POLLY_REGISTER_PASSES_H
namespace llvm {
  class PassManagerBase;
}

namespace polly {
// Register the Polly preoptimization passes. Preoptimizations are used to
// prepare the LLVM-IR for Polly. They increase the amount of code that can be
// optimized.
// (These passes are automatically included in registerPollyPasses).
void registerPollyPreoptPasses(llvm::PassManagerBase &PM);
}
#endif
