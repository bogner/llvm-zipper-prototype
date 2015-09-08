//===---------- TempScopInfo.cpp  - Extract TempScops ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Collect information about the control flow regions detected by the Scop
// detection, such that this information can be translated info its polyhedral
// representation.
//
//===----------------------------------------------------------------------===//

#include "polly/TempScopInfo.h"
#include "polly/Options.h"
#include "polly/CodeGen/BlockGenerators.h"
#include "polly/LinkAllPasses.h"
#include "polly/ScopDetection.h"
#include "polly/Support/GICHelper.h"
#include "polly/Support/SCEVValidator.h"
#include "polly/Support/ScopHelper.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/RegionIterator.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace polly;

static cl::opt<bool> ModelReadOnlyScalars(
    "polly-analyze-read-only-scalars",
    cl::desc("Model read-only scalar values in the scop description"),
    cl::Hidden, cl::ZeroOrMore, cl::init(true), cl::cat(PollyCategory));

#define DEBUG_TYPE "polly-analyze-ir"

//===----------------------------------------------------------------------===//
/// Helper Classes

void IRAccess::print(raw_ostream &OS) const {
  if (isRead())
    OS << "Read ";
  else {
    if (isMayWrite())
      OS << "May";
    OS << "Write ";
  }
  OS << BaseAddress->getName() << '[' << *Offset << "]\n";
}

void Comparison::print(raw_ostream &OS) const {
  // Not yet implemented.
}

//===----------------------------------------------------------------------===//
// TempScop implementation
TempScop::~TempScop() {}

void TempScop::print(raw_ostream &OS, ScalarEvolution *SE, LoopInfo *LI) const {
  OS << "Scop: " << R.getNameStr() << "\n";

  printDetail(OS, SE, LI, &R, 0);
}

void TempScop::printDetail(raw_ostream &OS, ScalarEvolution *SE, LoopInfo *LI,
                           const Region *CurR, unsigned ind) const {
  // FIXME: Print other details rather than memory accesses.
  for (const auto &CurBlock : CurR->blocks()) {
    AccFuncMapType::const_iterator AccSetIt = AccFuncMap.find(CurBlock);

    // Ignore trivial blocks that do not contain any memory access.
    if (AccSetIt == AccFuncMap.end())
      continue;

    OS.indent(ind) << "BB: " << CurBlock->getName() << '\n';
    typedef AccFuncSetType::const_iterator access_iterator;
    const AccFuncSetType &AccFuncs = AccSetIt->second;

    for (access_iterator AI = AccFuncs.begin(), AE = AccFuncs.end(); AI != AE;
         ++AI)
      AI->first.print(OS.indent(ind + 2));
  }
}

void TempScopInfo::buildPHIAccesses(PHINode *PHI, Region &R,
                                    AccFuncSetType &Functions,
                                    Region *NonAffineSubRegion,
                                    bool IsExitBlock) {

  // PHI nodes that are in the exit block of the region, hence if IsExitBlock is
  // true, are not modeled as ordinary PHI nodes as they are not part of the
  // region. However, we model the operands in the predecessor blocks that are
  // part of the region as regular scalar accesses.

  // If we can synthesize a PHI we can skip it, however only if it is in
  // the region. If it is not it can only be in the exit block of the region.
  // In this case we model the operands but not the PHI itself.
  if (!IsExitBlock && canSynthesize(PHI, LI, SE, &R))
    return;

  // PHI nodes are modeled as if they had been demoted prior to the SCoP
  // detection. Hence, the PHI is a load of a new memory location in which the
  // incoming value was written at the end of the incoming basic block.
  bool OnlyNonAffineSubRegionOperands = true;
  for (unsigned u = 0; u < PHI->getNumIncomingValues(); u++) {
    Value *Op = PHI->getIncomingValue(u);
    BasicBlock *OpBB = PHI->getIncomingBlock(u);

    // Do not build scalar dependences inside a non-affine subregion.
    if (NonAffineSubRegion && NonAffineSubRegion->contains(OpBB))
      continue;

    OnlyNonAffineSubRegionOperands = false;

    if (!R.contains(OpBB))
      continue;

    Instruction *OpI = dyn_cast<Instruction>(Op);
    if (OpI) {
      BasicBlock *OpIBB = OpI->getParent();
      // As we pretend there is a use (or more precise a write) of OpI in OpBB
      // we have to insert a scalar dependence from the definition of OpI to
      // OpBB if the definition is not in OpBB.
      if (OpIBB != OpBB) {
        IRAccess ScalarRead(IRAccess::READ, OpI, ZeroOffset, 1, true, OpI);
        AccFuncMap[OpBB].push_back(std::make_pair(ScalarRead, PHI));
        IRAccess ScalarWrite(IRAccess::MUST_WRITE, OpI, ZeroOffset, 1, true,
                             OpI);
        AccFuncMap[OpIBB].push_back(std::make_pair(ScalarWrite, OpI));
      }
    }

    // Always use the terminator of the incoming basic block as the access
    // instruction.
    OpI = OpBB->getTerminator();

    IRAccess ScalarAccess(IRAccess::MUST_WRITE, PHI, ZeroOffset, 1, true, Op,
                          /* IsPHI */ !IsExitBlock);
    AccFuncMap[OpBB].push_back(std::make_pair(ScalarAccess, OpI));
  }

  if (!OnlyNonAffineSubRegionOperands) {
    IRAccess ScalarAccess(IRAccess::READ, PHI, ZeroOffset, 1, true, PHI,
                          /* IsPHI */ !IsExitBlock);
    Functions.push_back(std::make_pair(ScalarAccess, PHI));
  }
}

bool TempScopInfo::buildScalarDependences(Instruction *Inst, Region *R,
                                          Region *NonAffineSubRegion) {
  bool canSynthesizeInst = canSynthesize(Inst, LI, SE, R);
  if (isIgnoredIntrinsic(Inst))
    return false;

  bool AnyCrossStmtUse = false;
  BasicBlock *ParentBB = Inst->getParent();

  for (User *U : Inst->users()) {
    Instruction *UI = dyn_cast<Instruction>(U);

    // Ignore the strange user
    if (UI == 0)
      continue;

    BasicBlock *UseParent = UI->getParent();

    // Ignore the users in the same BB (statement)
    if (UseParent == ParentBB)
      continue;

    // Do not build scalar dependences inside a non-affine subregion.
    if (NonAffineSubRegion && NonAffineSubRegion->contains(UseParent))
      continue;

    // Check whether or not the use is in the SCoP.
    if (!R->contains(UseParent)) {
      AnyCrossStmtUse = true;
      continue;
    }

    // If the instruction can be synthesized and the user is in the region
    // we do not need to add scalar dependences.
    if (canSynthesizeInst)
      continue;

    // No need to translate these scalar dependences into polyhedral form,
    // because synthesizable scalars can be generated by the code generator.
    if (canSynthesize(UI, LI, SE, R))
      continue;

    // Skip PHI nodes in the region as they handle their operands on their own.
    if (isa<PHINode>(UI))
      continue;

    // Now U is used in another statement.
    AnyCrossStmtUse = true;

    // Do not build a read access that is not in the current SCoP
    // Use the def instruction as base address of the IRAccess, so that it will
    // become the name of the scalar access in the polyhedral form.
    IRAccess ScalarAccess(IRAccess::READ, Inst, ZeroOffset, 1, true, Inst);
    AccFuncMap[UseParent].push_back(std::make_pair(ScalarAccess, UI));
  }

  if (ModelReadOnlyScalars) {
    for (Value *Op : Inst->operands()) {
      if (canSynthesize(Op, LI, SE, R))
        continue;

      if (Instruction *OpInst = dyn_cast<Instruction>(Op))
        if (R->contains(OpInst))
          continue;

      if (isa<Constant>(Op))
        continue;

      IRAccess ScalarAccess(IRAccess::READ, Op, ZeroOffset, 1, true, Op);
      AccFuncMap[Inst->getParent()].push_back(
          std::make_pair(ScalarAccess, Inst));
    }
  }

  return AnyCrossStmtUse;
}

extern MapInsnToMemAcc InsnToMemAcc;

IRAccess
TempScopInfo::buildIRAccess(Instruction *Inst, Loop *L, Region *R,
                            const ScopDetection::BoxedLoopsSetTy *BoxedLoops) {
  unsigned Size;
  Type *SizeType;
  Value *Val;
  enum IRAccess::TypeKind Type;

  if (LoadInst *Load = dyn_cast<LoadInst>(Inst)) {
    SizeType = Load->getType();
    Size = TD->getTypeStoreSize(SizeType);
    Type = IRAccess::READ;
    Val = Load;
  } else {
    StoreInst *Store = cast<StoreInst>(Inst);
    SizeType = Store->getValueOperand()->getType();
    Size = TD->getTypeStoreSize(SizeType);
    Type = IRAccess::MUST_WRITE;
    Val = Store->getValueOperand();
  }

  const SCEV *AccessFunction = SE->getSCEVAtScope(getPointerOperand(*Inst), L);
  const SCEVUnknown *BasePointer =
      dyn_cast<SCEVUnknown>(SE->getPointerBase(AccessFunction));

  assert(BasePointer && "Could not find base pointer");
  AccessFunction = SE->getMinusSCEV(AccessFunction, BasePointer);

  auto AccItr = InsnToMemAcc.find(Inst);
  if (PollyDelinearize && AccItr != InsnToMemAcc.end())
    return IRAccess(Type, BasePointer->getValue(), AccessFunction, Size, true,
                    AccItr->second.DelinearizedSubscripts,
                    AccItr->second.Shape->DelinearizedSizes, Val);

  // Check if the access depends on a loop contained in a non-affine subregion.
  bool isVariantInNonAffineLoop = false;
  if (BoxedLoops) {
    SetVector<const Loop *> Loops;
    findLoops(AccessFunction, Loops);
    for (const Loop *L : Loops)
      if (BoxedLoops->count(L))
        isVariantInNonAffineLoop = true;
  }

  bool IsAffine = !isVariantInNonAffineLoop &&
                  isAffineExpr(R, AccessFunction, *SE, BasePointer->getValue());

  SmallVector<const SCEV *, 4> Subscripts, Sizes;
  Subscripts.push_back(AccessFunction);
  Sizes.push_back(SE->getConstant(ZeroOffset->getType(), Size));

  if (!IsAffine && Type == IRAccess::MUST_WRITE)
    Type = IRAccess::MAY_WRITE;

  return IRAccess(Type, BasePointer->getValue(), AccessFunction, Size, IsAffine,
                  Subscripts, Sizes, Val);
}

void TempScopInfo::buildAccessFunctions(Region &R, Region &SR) {

  if (SD->isNonAffineSubRegion(&SR, &R)) {
    for (BasicBlock *BB : SR.blocks())
      buildAccessFunctions(R, *BB, &SR);
    return;
  }

  for (auto I = SR.element_begin(), E = SR.element_end(); I != E; ++I)
    if (I->isSubRegion())
      buildAccessFunctions(R, *I->getNodeAs<Region>());
    else
      buildAccessFunctions(R, *I->getNodeAs<BasicBlock>());
}

void TempScopInfo::buildAccessFunctions(Region &R, BasicBlock &BB,
                                        Region *NonAffineSubRegion,
                                        bool IsExitBlock) {
  AccFuncSetType Functions;
  Loop *L = LI->getLoopFor(&BB);

  // The set of loops contained in non-affine subregions that are part of R.
  const ScopDetection::BoxedLoopsSetTy *BoxedLoops = SD->getBoxedLoops(&R);

  for (BasicBlock::iterator I = BB.begin(), E = --BB.end(); I != E; ++I) {
    Instruction *Inst = I;

    PHINode *PHI = dyn_cast<PHINode>(Inst);
    if (PHI)
      buildPHIAccesses(PHI, R, Functions, NonAffineSubRegion, IsExitBlock);

    // For the exit block we stop modeling after the last PHI node.
    if (!PHI && IsExitBlock)
      break;

    if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst))
      Functions.push_back(
          std::make_pair(buildIRAccess(Inst, L, &R, BoxedLoops), Inst));

    if (isIgnoredIntrinsic(Inst))
      continue;

    if (buildScalarDependences(Inst, &R, NonAffineSubRegion)) {
      // If the Instruction is used outside the statement, we need to build the
      // write access.
      if (!isa<StoreInst>(Inst)) {
        IRAccess ScalarAccess(IRAccess::MUST_WRITE, Inst, ZeroOffset, 1, true,
                              Inst);
        Functions.push_back(std::make_pair(ScalarAccess, Inst));
      }
    }
  }

  if (Functions.empty())
    return;

  AccFuncSetType &Accs = AccFuncMap[&BB];
  Accs.insert(Accs.end(), Functions.begin(), Functions.end());
}

TempScop *TempScopInfo::buildTempScop(Region &R) {
  TempScop *TScop = new TempScop(R, AccFuncMap);

  buildAccessFunctions(R, R);

  // In case the region does not have an exiting block we will later (during
  // code generation) split the exit block. This will move potential PHI nodes
  // from the current exit block into the new region exiting block. Hence, PHI
  // nodes that are at this point not part of the region will be.
  // To handle these PHI nodes later we will now model their operands as scalar
  // accesses. Note that we do not model anything in the exit block if we have
  // an exiting block in the region, as there will not be any splitting later.
  if (!R.getExitingBlock())
    buildAccessFunctions(R, *R.getExit(), nullptr, /* IsExitBlock */ true);

  return TScop;
}

TempScop *TempScopInfo::getTempScop() const { return TempScopOfRegion; }

void TempScopInfo::print(raw_ostream &OS, const Module *) const {
  if (TempScopOfRegion)
    TempScopOfRegion->print(OS, SE, LI);
}

bool TempScopInfo::runOnRegion(Region *R, RGPassManager &RGM) {
  SD = &getAnalysis<ScopDetection>();

  if (!SD->isMaxRegionInScop(*R))
    return false;

  Function *F = R->getEntry()->getParent();
  SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  AA = &getAnalysis<AliasAnalysis>();
  TD = &F->getParent()->getDataLayout();
  ZeroOffset = SE->getConstant(TD->getIntPtrType(F->getContext()), 0);

  assert(!TempScopOfRegion && "Build the TempScop only once");
  TempScopOfRegion = buildTempScop(*R);

  return false;
}

void TempScopInfo::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<ScalarEvolutionWrapperPass>();
  AU.addRequiredTransitive<ScopDetection>();
  AU.addRequiredID(IndependentBlocksID);
  AU.addRequired<AliasAnalysis>();
  AU.setPreservesAll();
}

TempScopInfo::~TempScopInfo() { clear(); }

void TempScopInfo::clear() {
  AccFuncMap.clear();
  if (TempScopOfRegion)
    delete TempScopOfRegion;
  TempScopOfRegion = nullptr;
}

//===----------------------------------------------------------------------===//
// TempScop information extraction pass implement
char TempScopInfo::ID = 0;

Pass *polly::createTempScopInfoPass() { return new TempScopInfo(); }

INITIALIZE_PASS_BEGIN(TempScopInfo, "polly-analyze-ir",
                      "Polly - Analyse the LLVM-IR in the detected regions",
                      false, false);
INITIALIZE_AG_DEPENDENCY(AliasAnalysis);
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass);
INITIALIZE_PASS_DEPENDENCY(RegionInfoPass);
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass);
INITIALIZE_PASS_END(TempScopInfo, "polly-analyze-ir",
                    "Polly - Analyse the LLVM-IR in the detected regions",
                    false, false)
