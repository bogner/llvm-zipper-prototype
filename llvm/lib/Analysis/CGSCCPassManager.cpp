//===- CGSCCPassManager.cpp - Managing & running CGSCC passes -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

CGSCCAnalysisManagerModuleProxy::Result
CGSCCAnalysisManagerModuleProxy::run(Module &M) {
  assert(CGAM->empty() && "CGSCC analyses ran prior to the module proxy!");
  return Result(*CGAM);
}

CGSCCAnalysisManagerModuleProxy::Result::~Result() {
  // CGAM is cleared in a moved from state where there is nothing to do.
  if (!CGAM)
    return;

  // Clear out the analysis manager if we're being destroyed -- it means we
  // didn't even see an invalidate call when we got invalidated.
  CGAM->clear();
}

bool CGSCCAnalysisManagerModuleProxy::Result::invalidate(
    Module &M, const PreservedAnalyses &PA) {
  // If this proxy isn't marked as preserved, then we can't even invalidate
  // individual CGSCC analyses, there may be an invalid set of SCC objects in
  // the cache making it impossible to incrementally preserve them.
  // Just clear the entire manager.
  if (!PA.preserved(ID()))
    CGAM->clear();

  // Return false to indicate that this result is still a valid proxy.
  return false;
}

FunctionAnalysisManagerCGSCCProxy::Result
FunctionAnalysisManagerCGSCCProxy::run(LazyCallGraph::SCC &C) {
  return Result(*FAM);
}

FunctionAnalysisManagerCGSCCProxy::Result::~Result() {
  // FAM is cleared in a moved from state where there is nothing to do.
  if (!FAM)
    return;

  // Clear out the analysis manager if we're being destroyed -- it means we
  // didn't even see an invalidate call when we got invalidated.
  FAM->clear();
}

bool FunctionAnalysisManagerCGSCCProxy::Result::invalidate(
    LazyCallGraph::SCC &C, const PreservedAnalyses &PA) {
  // If this proxy isn't marked as preserved, then we can't even invalidate
  // individual function analyses, there may be an invalid set of Function
  // objects in the cache making it impossible to incrementally preserve them.
  // Just clear the entire manager.
  if (!PA.preserved(ID()))
    FAM->clear();

  // Return false to indicate that this result is still a valid proxy.
  return false;
}
