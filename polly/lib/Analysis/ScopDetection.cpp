//===----- ScopDetection.cpp  - Detect Scops --------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Detect the maximal Scops of a function.
//
// A static control part (Scop) is a subgraph of the control flow graph (CFG)
// that only has statically known control flow and can therefore be described
// within the polyhedral model.
//
// Every Scop fullfills these restrictions:
//
// * It is a single entry single exit region
//
// * Only affine linear bounds in the loops
//
// Every natural loop in a Scop must have a number of loop iterations that can
// be described as an affine linear function in surrounding loop iterators or
// parameters. (A parameter is a scalar that does not change its value during
// execution of the Scop).
//
// * Only comparisons of affine linear expressions in conditions
//
// * All loops and conditions perfectly nested
//
// The control flow needs to be structured such that it could be written using
// just 'for' and 'if' statements, without the need for any 'goto', 'break' or
// 'continue'.
//
// * Side effect free functions call
//
// Only function calls and intrinsics that do not have side effects are allowed
// (readnone).
//
// The Scop detection finds the largest Scops by checking if the largest
// region is a Scop. If this is not the case, its canonical subregions are
// checked until a region is a Scop. It is now tried to extend this Scop by
// creating a larger non canonical region.
//
//===----------------------------------------------------------------------===//

#include "polly/CodeGen/BlockGenerators.h"
#include "polly/LinkAllPasses.h"
#include "polly/Options.h"
#include "polly/ScopDetection.h"
#include "polly/Support/SCEVValidator.h"
#include "polly/Support/ScopHelper.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/RegionIterator.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/DebugInfo.h"
#include "llvm/IR/LLVMContext.h"

#define DEBUG_TYPE "polly-detect"
#include "llvm/Support/Debug.h"

#include <set>

using namespace llvm;
using namespace polly;

static cl::opt<bool>
DetectScopsWithoutLoops("polly-detect-scops-in-functions-without-loops",
                        cl::desc("Detect scops in functions without loops"),
                        cl::Hidden, cl::init(false), cl::cat(PollyCategory));

static cl::opt<bool>
DetectRegionsWithoutLoops("polly-detect-scops-in-regions-without-loops",
                          cl::desc("Detect scops in regions without loops"),
                          cl::Hidden, cl::init(false), cl::cat(PollyCategory));

static cl::opt<std::string>
OnlyFunction("polly-only-func", cl::desc("Only run on a single function"),
             cl::value_desc("function-name"), cl::ValueRequired, cl::init(""),
             cl::cat(PollyCategory));

static cl::opt<bool>
IgnoreAliasing("polly-ignore-aliasing",
               cl::desc("Ignore possible aliasing of the array bases"),
               cl::Hidden, cl::init(false), cl::cat(PollyCategory));

static cl::opt<bool>
ReportLevel("polly-report",
            cl::desc("Print information about the activities of Polly"),
            cl::init(false), cl::cat(PollyCategory));

static cl::opt<bool>
AllowNonAffine("polly-allow-nonaffine",
               cl::desc("Allow non affine access functions in arrays"),
               cl::Hidden, cl::init(false), cl::cat(PollyCategory));

//===----------------------------------------------------------------------===//
// Statistics.

STATISTIC(ValidRegion, "Number of regions that a valid part of Scop");

#define BADSCOP_STAT(NAME, DESC)                                               \
  STATISTIC(Bad##NAME##ForScop, "Number of bad regions for Scop: " DESC)

#define INVALID(NAME, MESSAGE)                                                 \
  do {                                                                         \
    std::string Buf;                                                           \
    raw_string_ostream fmt(Buf);                                               \
    fmt << MESSAGE;                                                            \
    fmt.flush();                                                               \
    LastFailure = Buf;                                                         \
    DEBUG(dbgs() << MESSAGE);                                                  \
    DEBUG(dbgs() << "\n");                                                     \
    assert(!Context.Verifying && #NAME);                                       \
    if (!Context.Verifying)                                                    \
      ++Bad##NAME##ForScop;                                                    \
    return false;                                                              \
  } while (0)

#define INVALID_NOVERIFY(NAME, MESSAGE)                                        \
  do {                                                                         \
    std::string Buf;                                                           \
    raw_string_ostream fmt(Buf);                                               \
    fmt << MESSAGE;                                                            \
    fmt.flush();                                                               \
    LastFailure = Buf;                                                         \
    DEBUG(dbgs() << MESSAGE);                                                  \
    DEBUG(dbgs() << "\n");                                                     \
    /* DISABLED: assert(!Context.Verifying && #NAME); */                       \
    if (!Context.Verifying)                                                    \
      ++Bad##NAME##ForScop;                                                    \
    return false;                                                              \
  } while (0)

BADSCOP_STAT(CFG, "CFG too complex");
BADSCOP_STAT(IndVar, "Non canonical induction variable in loop");
BADSCOP_STAT(LoopBound, "Loop bounds can not be computed");
BADSCOP_STAT(FuncCall, "Function call with side effects appeared");
BADSCOP_STAT(AffFunc, "Expression not affine");
BADSCOP_STAT(Alias, "Found base address alias");
BADSCOP_STAT(SimpleLoop, "Loop not in -loop-simplify form");
BADSCOP_STAT(Other, "Others");

//===----------------------------------------------------------------------===//
// ScopDetection.
bool ScopDetection::isMaxRegionInScop(const Region &R) const {
  // The Region is valid only if it could be found in the set.
  return ValidRegions.count(&R);
}

std::string ScopDetection::regionIsInvalidBecause(const Region *R) const {
  if (!InvalidRegions.count(R))
    return "";

  return InvalidRegions.find(R)->second;
}

bool ScopDetection::isValidCFG(BasicBlock &BB,
                               DetectionContext &Context) const {
  Region &RefRegion = Context.CurRegion;
  TerminatorInst *TI = BB.getTerminator();

  // Return instructions are only valid if the region is the top level region.
  if (isa<ReturnInst>(TI) && !RefRegion.getExit() && TI->getNumOperands() == 0)
    return true;

  BranchInst *Br = dyn_cast<BranchInst>(TI);

  if (!Br)
    INVALID(CFG, "Non branch instruction terminates BB: " + BB.getName());

  if (Br->isUnconditional())
    return true;

  Value *Condition = Br->getCondition();

  // UndefValue is not allowed as condition.
  if (isa<UndefValue>(Condition))
    INVALID(AffFunc, "Condition based on 'undef' value in BB: " + BB.getName());

  // Only Constant and ICmpInst are allowed as condition.
  if (!(isa<Constant>(Condition) || isa<ICmpInst>(Condition)))
    INVALID(AffFunc, "Condition in BB '" + BB.getName() +
                         "' neither constant nor an icmp instruction");

  // Allow perfectly nested conditions.
  assert(Br->getNumSuccessors() == 2 && "Unexpected number of successors");

  if (ICmpInst *ICmp = dyn_cast<ICmpInst>(Condition)) {
    // Unsigned comparisons are not allowed. They trigger overflow problems
    // in the code generation.
    //
    // TODO: This is not sufficient and just hides bugs. However it does pretty
    // well.
    if (ICmp->isUnsigned())
      return false;

    // Are both operands of the ICmp affine?
    if (isa<UndefValue>(ICmp->getOperand(0)) ||
        isa<UndefValue>(ICmp->getOperand(1)))
      INVALID(AffFunc, "undef operand in branch at BB: " + BB.getName());

    Loop *L = LI->getLoopFor(ICmp->getParent());
    const SCEV *LHS = SE->getSCEVAtScope(ICmp->getOperand(0), L);
    const SCEV *RHS = SE->getSCEVAtScope(ICmp->getOperand(1), L);

    if (!isAffineExpr(&Context.CurRegion, LHS, *SE) ||
        !isAffineExpr(&Context.CurRegion, RHS, *SE))
      INVALID(AffFunc, "Non affine branch in BB '" << BB.getName()
                                                   << "' with LHS: " << *LHS
                                                   << " and RHS: " << *RHS);
  }

  // Allow loop exit conditions.
  Loop *L = LI->getLoopFor(&BB);
  if (L && L->getExitingBlock() == &BB)
    return true;

  // Allow perfectly nested conditions.
  Region *R = RI->getRegionFor(&BB);
  if (R->getEntry() != &BB)
    INVALID(CFG, "Not well structured condition at BB: " + BB.getName());

  return true;
}

bool ScopDetection::isValidCallInst(CallInst &CI) {
  if (CI.mayHaveSideEffects() || CI.doesNotReturn())
    return false;

  if (CI.doesNotAccessMemory())
    return true;

  Function *CalledFunction = CI.getCalledFunction();

  // Indirect calls are not supported.
  if (CalledFunction == 0)
    return false;

  // TODO: Intrinsics.
  return false;
}

bool ScopDetection::isValidMemoryAccess(Instruction &Inst,
                                        DetectionContext &Context) const {
  Value *Ptr = getPointerOperand(Inst);
  Loop *L = LI->getLoopFor(Inst.getParent());
  const SCEV *AccessFunction = SE->getSCEVAtScope(Ptr, L);
  const SCEVUnknown *BasePointer;
  Value *BaseValue;

  BasePointer = dyn_cast<SCEVUnknown>(SE->getPointerBase(AccessFunction));

  if (!BasePointer)
    INVALID(AffFunc, "No base pointer");

  BaseValue = BasePointer->getValue();

  if (isa<UndefValue>(BaseValue))
    INVALID(AffFunc, "Undefined base pointer");

  AccessFunction = SE->getMinusSCEV(AccessFunction, BasePointer);

  if (!isAffineExpr(&Context.CurRegion, AccessFunction, *SE, BaseValue) &&
      !AllowNonAffine)
    INVALID(AffFunc, "Non affine access function: " << *AccessFunction);

  // FIXME: Alias Analysis thinks IntToPtrInst aliases with alloca instructions
  // created by IndependentBlocks Pass.
  if (isa<IntToPtrInst>(BaseValue))
    INVALID(Other, "Find bad intToptr prt: " << *BaseValue);

  if (!IgnoreAliasing) {
    // Check if the base pointer of the memory access does alias with
    // any other pointer. This cannot be handled at the moment.
    AliasSet &AS =
        Context.AST.getAliasSetForPointer(BaseValue, AliasAnalysis::UnknownSize,
                                          Inst.getMetadata(LLVMContext::MD_tbaa));

    // INVALID triggers an assertion in verifying mode, if it detects that a SCoP
    // was detected by SCoP detection and that this SCoP was invalidated by a pass
    // that stated it would preserve the SCoPs.
    // We disable this check as the independent blocks pass may create memory
    // references which seem to alias, if -basicaa is not available. They actually
    // do not, but as we can not proof this without -basicaa we would fail. We
    // disable this check to not cause irrelevant verification failures.
    if (!AS.isMustAlias()) {
      std::string Message;
      raw_string_ostream OS(Message);

      OS << "Possible aliasing: ";

      std::vector<Value *> Pointers;

      for (AliasSet::iterator AI = AS.begin(), AE = AS.end(); AI != AE; ++AI)
        Pointers.push_back(AI.getPointer());

      std::sort(Pointers.begin(), Pointers.end());

      for (std::vector<Value *>::iterator PI = Pointers.begin(),
          PE = Pointers.end(); ;) {
        Value *V = *PI;

        if (V->getName().size() == 0)
          OS << "\"" << *V << "\"";
        else
          OS << "\"" << V->getName() << "\"";

        ++PI;

        if (PI != PE)
          OS << ", ";
        else
          break;
      }

      INVALID_NOVERIFY(Alias, OS.str());
    }
  }

  return true;
}

bool ScopDetection::isValidInstruction(Instruction &Inst,
                                       DetectionContext &Context) const {
  if (PHINode *PN = dyn_cast<PHINode>(&Inst))
    if (!canSynthesize(PN, LI, SE, &Context.CurRegion)) {
      if (SCEVCodegen)
        INVALID(IndVar,
                "SCEV of PHI node refers to SSA names in region: " << Inst);
      else
        INVALID(IndVar, "Non canonical PHI node: " << Inst);
    }

  // We only check the call instruction but not invoke instruction.
  if (CallInst *CI = dyn_cast<CallInst>(&Inst)) {
    if (isValidCallInst(*CI))
      return true;

    INVALID(FuncCall, "Call instruction: " << Inst);
  }

  if (!Inst.mayWriteToMemory() && !Inst.mayReadFromMemory()) {
    if (isa<AllocaInst>(Inst))
      INVALID(Other, "Alloca instruction: " << Inst);

    return true;
  }

  // Check the access function.
  if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst))
    return isValidMemoryAccess(Inst, Context);

  // We do not know this instruction, therefore we assume it is invalid.
  INVALID(Other, "Unknown instruction: " << Inst);
}

bool ScopDetection::isValidBasicBlock(BasicBlock &BB,
                                      DetectionContext &Context) const {
  // Check all instructions, except the terminator instruction.
  for (BasicBlock::iterator I = BB.begin(), E = --BB.end(); I != E; ++I)
    if (!isValidInstruction(*I, Context))
      return false;

  return true;
}

bool ScopDetection::isValidLoop(Loop *L, DetectionContext &Context) const {
  if (!SCEVCodegen) {
    // If code generation is not in scev based mode, we need to ensure that
    // each loop has a canonical induction variable.
    PHINode *IndVar = L->getCanonicalInductionVariable();
    if (!IndVar)
      INVALID(IndVar,
              "No canonical IV at loop header: " << L->getHeader()->getName());
  }

  // Is the loop count affine?
  const SCEV *LoopCount = SE->getBackedgeTakenCount(L);
  if (!isAffineExpr(&Context.CurRegion, LoopCount, *SE))
    INVALID(LoopBound,
            "Non affine loop bound '"
                << *LoopCount << "' in loop: " << L->getHeader()->getName());

  return true;
}

Region *ScopDetection::expandRegion(Region &R) {
  // Initial no valid region was found (greater than R)
  Region *LastValidRegion = NULL;
  Region *ExpandedRegion = R.getExpandedRegion();

  DEBUG(dbgs() << "\tExpanding " << R.getNameStr() << "\n");

  while (ExpandedRegion) {
    DetectionContext Context(*ExpandedRegion, *AA, false /* verifying */);
    DEBUG(dbgs() << "\t\tTrying " << ExpandedRegion->getNameStr() << "\n");

    // Check the exit first (cheap)
    if (isValidExit(Context)) {
      // If the exit is valid check all blocks
      //  - if true, a valid region was found => store it + keep expanding
      //  - if false, .tbd. => stop  (should this really end the loop?)
      if (!allBlocksValid(Context))
        break;

      // Delete unnecessary regions (allocated by getExpandedRegion)
      if (LastValidRegion)
        delete LastValidRegion;

      // Store this region, because it is the greatest valid (encountered so
      // far).
      LastValidRegion = ExpandedRegion;

      // Create and test the next greater region (if any)
      ExpandedRegion = ExpandedRegion->getExpandedRegion();

    } else {
      // Create and test the next greater region (if any)
      Region *TmpRegion = ExpandedRegion->getExpandedRegion();

      // Delete unnecessary regions (allocated by getExpandedRegion)
      delete ExpandedRegion;

      ExpandedRegion = TmpRegion;
    }
  }

  DEBUG(if (LastValidRegion)
            dbgs() << "\tto " << LastValidRegion->getNameStr() << "\n";
        else dbgs() << "\tExpanding " << R.getNameStr() << " failed\n";);

  return LastValidRegion;
}
static bool regionWithoutLoops(Region &R, LoopInfo *LI) {
  for (Region::block_iterator I = R.block_begin(), E = R.block_end(); I != E;
       ++I)
    if (R.contains(LI->getLoopFor(*I)))
      return false;

  return true;
}

void ScopDetection::findScops(Region &R) {

  if (!DetectRegionsWithoutLoops && regionWithoutLoops(R, LI))
    return;

  DetectionContext Context(R, *AA, false /*verifying*/);

  LastFailure = "";

  if (isValidRegion(Context)) {
    ++ValidRegion;
    ValidRegions.insert(&R);
    return;
  }

  InvalidRegions[&R] = LastFailure;

  for (Region::iterator I = R.begin(), E = R.end(); I != E; ++I)
    findScops(**I);

  // Try to expand regions.
  //
  // As the region tree normally only contains canonical regions, non canonical
  // regions that form a Scop are not found. Therefore, those non canonical
  // regions are checked by expanding the canonical ones.

  std::vector<Region *> ToExpand;

  for (Region::iterator I = R.begin(), E = R.end(); I != E; ++I)
    ToExpand.push_back(*I);

  for (std::vector<Region *>::iterator RI = ToExpand.begin(),
                                       RE = ToExpand.end();
       RI != RE; ++RI) {
    Region *CurrentRegion = *RI;

    // Skip invalid regions. Regions may become invalid, if they are element of
    // an already expanded region.
    if (ValidRegions.find(CurrentRegion) == ValidRegions.end())
      continue;

    Region *ExpandedR = expandRegion(*CurrentRegion);

    if (!ExpandedR)
      continue;

    R.addSubRegion(ExpandedR, true);
    ValidRegions.insert(ExpandedR);
    ValidRegions.erase(CurrentRegion);

    for (Region::iterator I = ExpandedR->begin(), E = ExpandedR->end(); I != E;
         ++I)
      ValidRegions.erase(*I);
  }
}

bool ScopDetection::allBlocksValid(DetectionContext &Context) const {
  Region &R = Context.CurRegion;

  for (Region::block_iterator I = R.block_begin(), E = R.block_end(); I != E;
       ++I) {
    Loop *L = LI->getLoopFor(*I);
    if (L && L->getHeader() == *I && !isValidLoop(L, Context))
      return false;
  }

  for (Region::block_iterator I = R.block_begin(), E = R.block_end(); I != E;
       ++I)
    if (!isValidCFG(**I, Context))
      return false;

  for (Region::block_iterator I = R.block_begin(), E = R.block_end(); I != E;
       ++I)
    if (!isValidBasicBlock(**I, Context))
      return false;

  return true;
}

bool ScopDetection::isValidExit(DetectionContext &Context) const {
  Region &R = Context.CurRegion;

  // PHI nodes are not allowed in the exit basic block.
  if (BasicBlock *Exit = R.getExit()) {
    BasicBlock::iterator I = Exit->begin();
    if (I != Exit->end() && isa<PHINode>(*I))
      INVALID(Other, "PHI node in exit BB");
  }

  return true;
}

bool ScopDetection::isValidRegion(DetectionContext &Context) const {
  Region &R = Context.CurRegion;

  DEBUG(dbgs() << "Checking region: " << R.getNameStr() << "\n\t");

  // The toplevel region is no valid region.
  if (R.isTopLevelRegion()) {
    DEBUG(dbgs() << "Top level region is invalid"; dbgs() << "\n");
    return false;
  }

  if (!R.getEnteringBlock()) {
    Loop *L = LI->getLoopFor(R.getEntry());
    if (L && !L->isLoopSimplifyForm())
      INVALID(SimpleLoop, "Loop not in simplify form is invalid!");
  }

  // SCoP cannot contain the entry block of the function, because we need
  // to insert alloca instruction there when translate scalar to array.
  if (R.getEntry() == &(R.getEntry()->getParent()->getEntryBlock()))
    INVALID(Other, "Region containing entry block of function is invalid!");

  if (!isValidExit(Context))
    return false;

  if (!allBlocksValid(Context))
    return false;

  DEBUG(dbgs() << "OK\n");
  return true;
}

bool ScopDetection::isValidFunction(llvm::Function &F) {
  return !InvalidFunctions.count(&F);
}

void ScopDetection::getDebugLocation(const Region *R, unsigned &LineBegin,
                                     unsigned &LineEnd, std::string &FileName) {
  LineBegin = -1;
  LineEnd = 0;

  for (Region::const_block_iterator RI = R->block_begin(), RE = R->block_end();
       RI != RE; ++RI)
    for (BasicBlock::iterator BI = (*RI)->begin(), BE = (*RI)->end(); BI != BE;
         ++BI) {
      DebugLoc DL = BI->getDebugLoc();
      if (DL.isUnknown())
        continue;

      DIScope Scope(DL.getScope(BI->getContext()));

      if (FileName.empty())
        FileName = Scope.getFilename();

      unsigned NewLine = DL.getLine();

      LineBegin = std::min(LineBegin, NewLine);
      LineEnd = std::max(LineEnd, NewLine);
      break;
    }
}

void ScopDetection::printLocations(llvm::Function &F) {
  int NumberOfScops = std::distance(begin(), end());

  if (NumberOfScops)
    outs() << ":: Static control regions in " << F.getName() << "\n";

  for (iterator RI = begin(), RE = end(); RI != RE; ++RI) {
    unsigned LineEntry, LineExit;
    std::string FileName;

    getDebugLocation(*RI, LineEntry, LineExit, FileName);

    if (FileName.empty()) {
      outs() << "Scop detected at unknown location. Compile with debug info "
                "(-g) to get more precise information. \n";
      return;
    }

    outs() << FileName << ":" << LineEntry
           << ": Start of static control region\n";
    outs() << FileName << ":" << LineExit << ": End of static control region\n";
  }
}

bool ScopDetection::runOnFunction(llvm::Function &F) {
  LI = &getAnalysis<LoopInfo>();
  if (!DetectScopsWithoutLoops && LI->empty())
    return false;

  AA = &getAnalysis<AliasAnalysis>();
  SE = &getAnalysis<ScalarEvolution>();
  RI = &getAnalysis<RegionInfo>();
  Region *TopRegion = RI->getTopLevelRegion();

  releaseMemory();

  if (OnlyFunction != "" && F.getName() != OnlyFunction)
    return false;

  if (!isValidFunction(F))
    return false;

  findScops(*TopRegion);

  if (ReportLevel >= 1)
    printLocations(F);

  return false;
}

void polly::ScopDetection::verifyRegion(const Region &R) const {
  assert(isMaxRegionInScop(R) && "Expect R is a valid region.");
  DetectionContext Context(const_cast<Region &>(R), *AA, true /*verifying*/);
  isValidRegion(Context);
}

void polly::ScopDetection::verifyAnalysis() const {
  for (RegionSet::const_iterator I = ValidRegions.begin(),
                                 E = ValidRegions.end();
       I != E; ++I)
    verifyRegion(**I);
}

void ScopDetection::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTree>();
  AU.addRequired<PostDominatorTree>();
  AU.addRequired<LoopInfo>();
  AU.addRequired<ScalarEvolution>();
  // We also need AA and RegionInfo when we are verifying analysis.
  AU.addRequiredTransitive<AliasAnalysis>();
  AU.addRequiredTransitive<RegionInfo>();
  AU.setPreservesAll();
}

void ScopDetection::print(raw_ostream &OS, const Module *) const {
  for (RegionSet::const_iterator I = ValidRegions.begin(),
                                 E = ValidRegions.end();
       I != E; ++I)
    OS << "Valid Region for Scop: " << (*I)->getNameStr() << '\n';

  OS << "\n";
}

void ScopDetection::releaseMemory() {
  ValidRegions.clear();
  InvalidRegions.clear();
  // Do not clear the invalid function set.
}

char ScopDetection::ID = 0;

Pass *polly::createScopDetectionPass() { return new ScopDetection(); }

INITIALIZE_PASS_BEGIN(ScopDetection, "polly-detect",
                      "Polly - Detect static control parts (SCoPs)", false,
                      false);
INITIALIZE_AG_DEPENDENCY(AliasAnalysis);
INITIALIZE_PASS_DEPENDENCY(DominatorTree);
INITIALIZE_PASS_DEPENDENCY(LoopInfo);
INITIALIZE_PASS_DEPENDENCY(PostDominatorTree);
INITIALIZE_PASS_DEPENDENCY(RegionInfo);
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution);
INITIALIZE_PASS_END(ScopDetection, "polly-detect",
                    "Polly - Detect static control parts (SCoPs)", false, false)
