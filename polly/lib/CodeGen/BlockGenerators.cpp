//===--- BlockGenerators.cpp - Generate code for statements -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the BlockGenerator and VectorBlockGenerator classes,
// which generate sequential code and vectorized code for a polyhedral
// statement, respectively.
//
//===----------------------------------------------------------------------===//

#include "polly/ScopInfo.h"
#include "polly/CodeGen/BlockGenerators.h"
#include "polly/CodeGen/CodeGeneration.h"
#include "polly/CodeGen/IslExprBuilder.h"
#include "polly/Options.h"
#include "polly/Support/GICHelper.h"
#include "polly/Support/SCEVValidator.h"
#include "polly/Support/ScopHelper.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/RegionInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "isl/aff.h"
#include "isl/ast.h"
#include "isl/ast_build.h"
#include "isl/set.h"
#include <deque>

using namespace llvm;
using namespace polly;

static cl::opt<bool> Aligned("enable-polly-aligned",
                             cl::desc("Assumed aligned memory accesses."),
                             cl::Hidden, cl::init(false), cl::ZeroOrMore,
                             cl::cat(PollyCategory));

bool polly::canSynthesize(const Value *V, const llvm::LoopInfo *LI,
                          ScalarEvolution *SE, const Region *R) {
  if (!V || !SE->isSCEVable(V->getType()))
    return false;

  if (const SCEV *Scev = SE->getSCEV(const_cast<Value *>(V)))
    if (!isa<SCEVCouldNotCompute>(Scev))
      if (!hasScalarDepsInsideRegion(Scev, R))
        return true;

  return false;
}

bool polly::isIgnoredIntrinsic(const Value *V) {
  if (auto *IT = dyn_cast<IntrinsicInst>(V)) {
    switch (IT->getIntrinsicID()) {
    // Lifetime markers are supported/ignored.
    case llvm::Intrinsic::lifetime_start:
    case llvm::Intrinsic::lifetime_end:
    // Invariant markers are supported/ignored.
    case llvm::Intrinsic::invariant_start:
    case llvm::Intrinsic::invariant_end:
    // Some misc annotations are supported/ignored.
    case llvm::Intrinsic::var_annotation:
    case llvm::Intrinsic::ptr_annotation:
    case llvm::Intrinsic::annotation:
    case llvm::Intrinsic::donothing:
    case llvm::Intrinsic::assume:
    case llvm::Intrinsic::expect:
    // Some debug info intrisics are supported/ignored.
    case llvm::Intrinsic::dbg_value:
    case llvm::Intrinsic::dbg_declare:
      return true;
    default:
      break;
    }
  }
  return false;
}

BlockGenerator::BlockGenerator(PollyIRBuilder &B, LoopInfo &LI,
                               ScalarEvolution &SE, DominatorTree &DT,
                               ScalarAllocaMapTy &ScalarMap,
                               ScalarAllocaMapTy &PHIOpMap,
                               EscapeUsersAllocaMapTy &EscapeMap,
                               IslExprBuilder *ExprBuilder)
    : Builder(B), LI(LI), SE(SE), ExprBuilder(ExprBuilder), DT(DT),
      EntryBB(nullptr), PHIOpMap(PHIOpMap), ScalarMap(ScalarMap),
      EscapeMap(EscapeMap) {}

Value *BlockGenerator::getNewValue(ScopStmt &Stmt, const Value *Old,
                                   ValueMapT &BBMap, ValueMapT &GlobalMap,
                                   LoopToScevMapT &LTS, Loop *L) const {
  // We assume constants never change.
  // This avoids map lookups for many calls to this function.
  if (isa<Constant>(Old))
    return const_cast<Value *>(Old);

  if (Value *New = GlobalMap.lookup(Old)) {
    if (Old->getType()->getScalarSizeInBits() <
        New->getType()->getScalarSizeInBits())
      New = Builder.CreateTruncOrBitCast(New, Old->getType());

    return New;
  }

  if (Value *New = BBMap.lookup(Old))
    return New;

  if (SE.isSCEVable(Old->getType()))
    if (const SCEV *Scev = SE.getSCEVAtScope(const_cast<Value *>(Old), L)) {
      if (!isa<SCEVCouldNotCompute>(Scev)) {
        const SCEV *NewScev = apply(Scev, LTS, SE);
        ValueToValueMap VTV;
        VTV.insert(BBMap.begin(), BBMap.end());
        VTV.insert(GlobalMap.begin(), GlobalMap.end());
        NewScev = SCEVParameterRewriter::rewrite(NewScev, SE, VTV);

        Scop &S = *Stmt.getParent();
        const DataLayout &DL =
            S.getRegion().getEntry()->getParent()->getParent()->getDataLayout();
        auto IP = Builder.GetInsertPoint();

        assert(IP != Builder.GetInsertBlock()->end() &&
               "Only instructions can be insert points for SCEVExpander");
        Value *Expanded =
            expandCodeFor(S, SE, DL, "polly", NewScev, Old->getType(), IP);

        BBMap[Old] = Expanded;
        return Expanded;
      }
    }

  // A scop-constant value defined by a global or a function parameter.
  if (isa<GlobalValue>(Old) || isa<Argument>(Old))
    return const_cast<Value *>(Old);

  // A scop-constant value defined by an instruction executed outside the scop.
  if (const Instruction *Inst = dyn_cast<Instruction>(Old))
    if (!Stmt.getParent()->getRegion().contains(Inst->getParent()))
      return const_cast<Value *>(Old);

  // The scalar dependence is neither available nor SCEVCodegenable.
  llvm_unreachable("Unexpected scalar dependence in region!");
  return nullptr;
}

void BlockGenerator::copyInstScalar(ScopStmt &Stmt, const Instruction *Inst,
                                    ValueMapT &BBMap, ValueMapT &GlobalMap,
                                    LoopToScevMapT &LTS) {
  // We do not generate debug intrinsics as we did not investigate how to
  // copy them correctly. At the current state, they just crash the code
  // generation as the meta-data operands are not correctly copied.
  if (isa<DbgInfoIntrinsic>(Inst))
    return;

  Instruction *NewInst = Inst->clone();

  // Replace old operands with the new ones.
  for (Value *OldOperand : Inst->operands()) {
    Value *NewOperand = getNewValue(Stmt, OldOperand, BBMap, GlobalMap, LTS,
                                    getLoopForInst(Inst));

    if (!NewOperand) {
      assert(!isa<StoreInst>(NewInst) &&
             "Store instructions are always needed!");
      delete NewInst;
      return;
    }

    NewInst->replaceUsesOfWith(OldOperand, NewOperand);
  }

  Builder.Insert(NewInst);
  BBMap[Inst] = NewInst;

  if (!NewInst->getType()->isVoidTy())
    NewInst->setName("p_" + Inst->getName());
}

Value *BlockGenerator::generateLocationAccessed(
    ScopStmt &Stmt, const Instruction *Inst, const Value *Pointer,
    ValueMapT &BBMap, ValueMapT &GlobalMap, LoopToScevMapT &LTS,
    isl_id_to_ast_expr *NewAccesses) {
  const MemoryAccess &MA = Stmt.getAccessFor(Inst);

  isl_ast_expr *AccessExpr = isl_id_to_ast_expr_get(NewAccesses, MA.getId());

  if (AccessExpr) {
    AccessExpr = isl_ast_expr_address_of(AccessExpr);
    return ExprBuilder->create(AccessExpr);
  }

  return getNewValue(Stmt, Pointer, BBMap, GlobalMap, LTS,
                     getLoopForInst(Inst));
}

Loop *BlockGenerator::getLoopForInst(const llvm::Instruction *Inst) {
  return LI.getLoopFor(Inst->getParent());
}

Value *BlockGenerator::generateScalarLoad(ScopStmt &Stmt, const LoadInst *Load,
                                          ValueMapT &BBMap,
                                          ValueMapT &GlobalMap,
                                          LoopToScevMapT &LTS,
                                          isl_id_to_ast_expr *NewAccesses) {
  const Value *Pointer = Load->getPointerOperand();
  Value *NewPointer = generateLocationAccessed(Stmt, Load, Pointer, BBMap,
                                               GlobalMap, LTS, NewAccesses);
  Value *ScalarLoad = Builder.CreateAlignedLoad(
      NewPointer, Load->getAlignment(), Load->getName() + "_p_scalar_");
  return ScalarLoad;
}

void BlockGenerator::generateScalarStore(ScopStmt &Stmt, const StoreInst *Store,
                                         ValueMapT &BBMap, ValueMapT &GlobalMap,
                                         LoopToScevMapT &LTS,
                                         isl_id_to_ast_expr *NewAccesses) {
  const Value *Pointer = Store->getPointerOperand();
  Value *NewPointer = generateLocationAccessed(Stmt, Store, Pointer, BBMap,
                                               GlobalMap, LTS, NewAccesses);
  Value *ValueOperand = getNewValue(Stmt, Store->getValueOperand(), BBMap,
                                    GlobalMap, LTS, getLoopForInst(Store));

  Builder.CreateAlignedStore(ValueOperand, NewPointer, Store->getAlignment());
}

void BlockGenerator::copyInstruction(ScopStmt &Stmt, const Instruction *Inst,
                                     ValueMapT &BBMap, ValueMapT &GlobalMap,
                                     LoopToScevMapT &LTS,
                                     isl_id_to_ast_expr *NewAccesses) {

  // First check for possible scalar dependences for this instruction.
  generateScalarLoads(Stmt, Inst, BBMap);

  // Terminator instructions control the control flow. They are explicitly
  // expressed in the clast and do not need to be copied.
  if (Inst->isTerminator())
    return;

  Loop *L = getLoopForInst(Inst);
  if ((Stmt.isBlockStmt() || !Stmt.getRegion()->contains(L)) &&
      canSynthesize(Inst, &LI, &SE, &Stmt.getParent()->getRegion())) {
    Value *NewValue = getNewValue(Stmt, Inst, BBMap, GlobalMap, LTS, L);
    BBMap[Inst] = NewValue;
    return;
  }

  if (const LoadInst *Load = dyn_cast<LoadInst>(Inst)) {
    Value *NewLoad =
        generateScalarLoad(Stmt, Load, BBMap, GlobalMap, LTS, NewAccesses);
    // Compute NewLoad before its insertion in BBMap to make the insertion
    // deterministic.
    BBMap[Load] = NewLoad;
    return;
  }

  if (const StoreInst *Store = dyn_cast<StoreInst>(Inst)) {
    generateScalarStore(Stmt, Store, BBMap, GlobalMap, LTS, NewAccesses);
    return;
  }

  if (const PHINode *PHI = dyn_cast<PHINode>(Inst)) {
    copyPHIInstruction(Stmt, PHI, BBMap, GlobalMap, LTS);
    return;
  }

  // Skip some special intrinsics for which we do not adjust the semantics to
  // the new schedule. All others are handled like every other instruction.
  if (isIgnoredIntrinsic(Inst))
    return;

  copyInstScalar(Stmt, Inst, BBMap, GlobalMap, LTS);
}

void BlockGenerator::copyStmt(ScopStmt &Stmt, ValueMapT &GlobalMap,
                              LoopToScevMapT &LTS,
                              isl_id_to_ast_expr *NewAccesses) {
  assert(Stmt.isBlockStmt() &&
         "Only block statements can be copied by the block generator");

  ValueMapT BBMap;

  BasicBlock *BB = Stmt.getBasicBlock();
  copyBB(Stmt, BB, BBMap, GlobalMap, LTS, NewAccesses);
}

BasicBlock *BlockGenerator::splitBB(BasicBlock *BB) {
  BasicBlock *CopyBB =
      SplitBlock(Builder.GetInsertBlock(), Builder.GetInsertPoint(), &DT, &LI);
  CopyBB->setName("polly.stmt." + BB->getName());
  return CopyBB;
}

BasicBlock *BlockGenerator::copyBB(ScopStmt &Stmt, BasicBlock *BB,
                                   ValueMapT &BBMap, ValueMapT &GlobalMap,
                                   LoopToScevMapT &LTS,
                                   isl_id_to_ast_expr *NewAccesses) {
  BasicBlock *CopyBB = splitBB(BB);
  copyBB(Stmt, BB, CopyBB, BBMap, GlobalMap, LTS, NewAccesses);
  return CopyBB;
}

void BlockGenerator::copyBB(ScopStmt &Stmt, BasicBlock *BB, BasicBlock *CopyBB,
                            ValueMapT &BBMap, ValueMapT &GlobalMap,
                            LoopToScevMapT &LTS,
                            isl_id_to_ast_expr *NewAccesses) {
  Builder.SetInsertPoint(CopyBB->begin());
  EntryBB = &CopyBB->getParent()->getEntryBlock();

  for (Instruction &Inst : *BB)
    copyInstruction(Stmt, &Inst, BBMap, GlobalMap, LTS, NewAccesses);

  // After a basic block was copied store all scalars that escape this block
  // in their alloca. First the scalars that have dependences inside the SCoP,
  // then the ones that might escape the SCoP.
  generateScalarStores(Stmt, BB, BBMap, GlobalMap);

  const Region &R = Stmt.getParent()->getRegion();
  for (Instruction &Inst : *BB)
    handleOutsideUsers(R, &Inst, BBMap[&Inst]);
}

AllocaInst *BlockGenerator::getOrCreateAlloca(Value *ScalarBase,
                                              ScalarAllocaMapTy &Map,
                                              const char *NameExt) {
  // Check if an alloca was cached for the base instruction.
  AllocaInst *&Addr = Map[ScalarBase];

  // If no alloca was found create one and insert it in the entry block.
  if (!Addr) {
    auto *Ty = ScalarBase->getType();
    Addr = new AllocaInst(Ty, ScalarBase->getName() + NameExt);
    Addr->insertBefore(EntryBB->getFirstInsertionPt());
  }

  return Addr;
}

AllocaInst *BlockGenerator::getOrCreateAlloca(MemoryAccess &Access) {
  if (Access.getScopArrayInfo()->isPHI())
    return getOrCreatePHIAlloca(Access.getBaseAddr());
  else
    return getOrCreateScalarAlloca(Access.getBaseAddr());
}

AllocaInst *BlockGenerator::getOrCreateScalarAlloca(Value *ScalarBase) {
  return getOrCreateAlloca(ScalarBase, ScalarMap, ".s2a");
}

AllocaInst *BlockGenerator::getOrCreatePHIAlloca(Value *ScalarBase) {
  return getOrCreateAlloca(ScalarBase, PHIOpMap, ".phiops");
}

void BlockGenerator::handleOutsideUsers(const Region &R, Instruction *Inst,
                                        Value *InstCopy) {
  // If there are escape users we get the alloca for this instruction and put it
  // in the EscapeMap for later finalization. Lastly, if the instruction was
  // copied multiple times we already did this and can exit.
  if (EscapeMap.count(Inst))
    return;

  EscapeUserVectorTy EscapeUsers;
  for (User *U : Inst->users()) {

    // Non-instruction user will never escape.
    Instruction *UI = dyn_cast<Instruction>(U);
    if (!UI)
      continue;

    if (R.contains(UI))
      continue;

    EscapeUsers.push_back(UI);
  }

  // Exit if no escape uses were found.
  if (EscapeUsers.empty())
    return;

  // Get or create an escape alloca for this instruction.
  AllocaInst *ScalarAddr = getOrCreateScalarAlloca(Inst);

  // Remember that this instruction has escape uses and the escape alloca.
  EscapeMap[Inst] = std::make_pair(ScalarAddr, std::move(EscapeUsers));
}

void BlockGenerator::generateScalarLoads(ScopStmt &Stmt,
                                         const Instruction *Inst,
                                         ValueMapT &BBMap) {
  auto *MAL = Stmt.lookupAccessesFor(Inst);

  if (!MAL)
    return;

  for (MemoryAccess &MA : *MAL) {
    if (!MA.isScalar() || !MA.isRead())
      continue;

    auto *Address = getOrCreateAlloca(MA);
    auto Name = Address->getName() + ".reload";
    BBMap[MA.getBaseAddr()] = Builder.CreateLoad(Address, Name);
  }
}

Value *BlockGenerator::getNewScalarValue(Value *ScalarValue, const Region &R,
                                         ValueMapT &BBMap,
                                         ValueMapT &GlobalMap) {
  // If the value we want to store is an instruction we might have demoted it
  // in order to make it accessible here. In such a case a reload is
  // necessary. If it is no instruction it will always be a value that
  // dominates the current point and we can just use it. In total there are 4
  // options:
  //  (1) The value is no instruction ==> use the value.
  //  (2) The value is an instruction that was split out of the region prior to
  //      code generation  ==> use the instruction as it dominates the region.
  //  (3) The value is an instruction:
  //      (a) The value was defined in the current block, thus a copy is in
  //          the BBMap ==> use the mapped value.
  //      (b) The value was defined in a previous block, thus we demoted it
  //          earlier ==> use the reloaded value.
  Instruction *ScalarValueInst = dyn_cast<Instruction>(ScalarValue);
  if (!ScalarValueInst)
    return ScalarValue;

  if (!R.contains(ScalarValueInst)) {
    if (Value *ScalarValueCopy = GlobalMap.lookup(ScalarValueInst))
      return /* Case (3a) */ ScalarValueCopy;
    else
      return /* Case 2 */ ScalarValue;
  }

  if (Value *ScalarValueCopy = BBMap.lookup(ScalarValueInst))
    return /* Case (3a) */ ScalarValueCopy;

  // Case (3b)
  Value *Address = getOrCreateScalarAlloca(ScalarValueInst);
  ScalarValue = Builder.CreateLoad(Address, Address->getName() + ".reload");

  return ScalarValue;
}

void BlockGenerator::generateScalarStores(ScopStmt &Stmt, BasicBlock *BB,
                                          ValueMapT &BBMap,
                                          ValueMapT &GlobalMap) {
  const Region &R = Stmt.getParent()->getRegion();

  assert(Stmt.isBlockStmt() && BB == Stmt.getBasicBlock() &&
         "Region statements need to use the generateScalarStores() "
         "function in the RegionGenerator");

  for (MemoryAccess *MA : Stmt) {
    if (!MA->isScalar() || MA->isRead())
      continue;

    Value *Val = MA->getAccessValue();
    auto *Address = getOrCreateAlloca(*MA);

    Val = getNewScalarValue(Val, R, BBMap, GlobalMap);
    Builder.CreateStore(Val, Address);
  }
}

void BlockGenerator::createScalarInitialization(Scop &S) {
  Region &R = S.getRegion();
  // The split block __just before__ the region and optimized region.
  BasicBlock *SplitBB = R.getEnteringBlock();
  BranchInst *SplitBBTerm = cast<BranchInst>(SplitBB->getTerminator());
  assert(SplitBBTerm->getNumSuccessors() == 2 && "Bad region entering block!");

  // Get the start block of the __optimized__ region.
  BasicBlock *StartBB = SplitBBTerm->getSuccessor(0);
  if (StartBB == R.getEntry())
    StartBB = SplitBBTerm->getSuccessor(1);

  Builder.SetInsertPoint(StartBB->getTerminator());

  for (auto &Pair : S.arrays()) {
    auto &Array = Pair.second;
    if (Array->getNumberOfDimensions() != 0)
      continue;
    if (Array->isPHI()) {
      // For PHI nodes, the only values we need to store are the ones that
      // reach the PHI node from outside the region. In general there should
      // only be one such incoming edge and this edge should enter through
      // 'SplitBB'.
      auto PHI = cast<PHINode>(Array->getBasePtr());

      for (auto BI = PHI->block_begin(), BE = PHI->block_end(); BI != BE; BI++)
        if (!R.contains(*BI) && *BI != SplitBB)
          llvm_unreachable("Incoming edges from outside the scop should always "
                           "come from SplitBB");

      int Idx = PHI->getBasicBlockIndex(SplitBB);
      if (Idx < 0)
        continue;

      Value *ScalarValue = PHI->getIncomingValue(Idx);

      Builder.CreateStore(ScalarValue, getOrCreatePHIAlloca(PHI));
      continue;
    }

    auto *Inst = dyn_cast<Instruction>(Array->getBasePtr());

    if (Inst && R.contains(Inst))
      continue;

    Builder.CreateStore(Array->getBasePtr(),
                        getOrCreateScalarAlloca(Array->getBasePtr()));
  }
}

void BlockGenerator::createScalarFinalization(Region &R) {
  // The exit block of the __unoptimized__ region.
  BasicBlock *ExitBB = R.getExitingBlock();
  // The merge block __just after__ the region and the optimized region.
  BasicBlock *MergeBB = R.getExit();

  // The exit block of the __optimized__ region.
  BasicBlock *OptExitBB = *(pred_begin(MergeBB));
  if (OptExitBB == ExitBB)
    OptExitBB = *(++pred_begin(MergeBB));

  Builder.SetInsertPoint(OptExitBB->getTerminator());
  for (const auto &EscapeMapping : EscapeMap) {
    // Extract the escaping instruction and the escaping users as well as the
    // alloca the instruction was demoted to.
    Instruction *EscapeInst = EscapeMapping.getFirst();
    const auto &EscapeMappingValue = EscapeMapping.getSecond();
    const EscapeUserVectorTy &EscapeUsers = EscapeMappingValue.second;
    AllocaInst *ScalarAddr = EscapeMappingValue.first;

    // Reload the demoted instruction in the optimized version of the SCoP.
    Instruction *EscapeInstReload =
        Builder.CreateLoad(ScalarAddr, EscapeInst->getName() + ".final_reload");

    // Create the merge PHI that merges the optimized and unoptimized version.
    PHINode *MergePHI = PHINode::Create(EscapeInst->getType(), 2,
                                        EscapeInst->getName() + ".merge");
    MergePHI->insertBefore(MergeBB->getFirstInsertionPt());

    // Add the respective values to the merge PHI.
    MergePHI->addIncoming(EscapeInstReload, OptExitBB);
    MergePHI->addIncoming(EscapeInst, ExitBB);

    // The information of scalar evolution about the escaping instruction needs
    // to be revoked so the new merged instruction will be used.
    if (SE.isSCEVable(EscapeInst->getType()))
      SE.forgetValue(EscapeInst);

    // Replace all uses of the demoted instruction with the merge PHI.
    for (Instruction *EUser : EscapeUsers)
      EUser->replaceUsesOfWith(EscapeInst, MergePHI);
  }
}

void BlockGenerator::finalizeSCoP(Scop &S) {
  createScalarInitialization(S);
  createScalarFinalization(S.getRegion());
}

VectorBlockGenerator::VectorBlockGenerator(BlockGenerator &BlockGen,
                                           VectorValueMapT &GlobalMaps,
                                           std::vector<LoopToScevMapT> &VLTS,
                                           isl_map *Schedule)
    : BlockGenerator(BlockGen), GlobalMaps(GlobalMaps), VLTS(VLTS),
      Schedule(Schedule) {
  assert(GlobalMaps.size() > 1 && "Only one vector lane found");
  assert(Schedule && "No statement domain provided");
}

Value *VectorBlockGenerator::getVectorValue(ScopStmt &Stmt, const Value *Old,
                                            ValueMapT &VectorMap,
                                            VectorValueMapT &ScalarMaps,
                                            Loop *L) {
  if (Value *NewValue = VectorMap.lookup(Old))
    return NewValue;

  int Width = getVectorWidth();

  Value *Vector = UndefValue::get(VectorType::get(Old->getType(), Width));

  for (int Lane = 0; Lane < Width; Lane++)
    Vector = Builder.CreateInsertElement(
        Vector, getNewValue(Stmt, Old, ScalarMaps[Lane], GlobalMaps[Lane],
                            VLTS[Lane], L),
        Builder.getInt32(Lane));

  VectorMap[Old] = Vector;

  return Vector;
}

Type *VectorBlockGenerator::getVectorPtrTy(const Value *Val, int Width) {
  PointerType *PointerTy = dyn_cast<PointerType>(Val->getType());
  assert(PointerTy && "PointerType expected");

  Type *ScalarType = PointerTy->getElementType();
  VectorType *VectorType = VectorType::get(ScalarType, Width);

  return PointerType::getUnqual(VectorType);
}

Value *VectorBlockGenerator::generateStrideOneLoad(
    ScopStmt &Stmt, const LoadInst *Load, VectorValueMapT &ScalarMaps,
    __isl_keep isl_id_to_ast_expr *NewAccesses, bool NegativeStride = false) {
  unsigned VectorWidth = getVectorWidth();
  const Value *Pointer = Load->getPointerOperand();
  Type *VectorPtrType = getVectorPtrTy(Pointer, VectorWidth);
  unsigned Offset = NegativeStride ? VectorWidth - 1 : 0;

  Value *NewPointer = nullptr;
  NewPointer =
      generateLocationAccessed(Stmt, Load, Pointer, ScalarMaps[Offset],
                               GlobalMaps[Offset], VLTS[Offset], NewAccesses);
  Value *VectorPtr =
      Builder.CreateBitCast(NewPointer, VectorPtrType, "vector_ptr");
  LoadInst *VecLoad =
      Builder.CreateLoad(VectorPtr, Load->getName() + "_p_vec_full");
  if (!Aligned)
    VecLoad->setAlignment(8);

  if (NegativeStride) {
    SmallVector<Constant *, 16> Indices;
    for (int i = VectorWidth - 1; i >= 0; i--)
      Indices.push_back(ConstantInt::get(Builder.getInt32Ty(), i));
    Constant *SV = llvm::ConstantVector::get(Indices);
    Value *RevVecLoad = Builder.CreateShuffleVector(
        VecLoad, VecLoad, SV, Load->getName() + "_reverse");
    return RevVecLoad;
  }

  return VecLoad;
}

Value *VectorBlockGenerator::generateStrideZeroLoad(
    ScopStmt &Stmt, const LoadInst *Load, ValueMapT &BBMap,
    __isl_keep isl_id_to_ast_expr *NewAccesses) {
  const Value *Pointer = Load->getPointerOperand();
  Type *VectorPtrType = getVectorPtrTy(Pointer, 1);
  Value *NewPointer = generateLocationAccessed(
      Stmt, Load, Pointer, BBMap, GlobalMaps[0], VLTS[0], NewAccesses);
  Value *VectorPtr = Builder.CreateBitCast(NewPointer, VectorPtrType,
                                           Load->getName() + "_p_vec_p");
  LoadInst *ScalarLoad =
      Builder.CreateLoad(VectorPtr, Load->getName() + "_p_splat_one");

  if (!Aligned)
    ScalarLoad->setAlignment(8);

  Constant *SplatVector = Constant::getNullValue(
      VectorType::get(Builder.getInt32Ty(), getVectorWidth()));

  Value *VectorLoad = Builder.CreateShuffleVector(
      ScalarLoad, ScalarLoad, SplatVector, Load->getName() + "_p_splat");
  return VectorLoad;
}

Value *VectorBlockGenerator::generateUnknownStrideLoad(
    ScopStmt &Stmt, const LoadInst *Load, VectorValueMapT &ScalarMaps,
    __isl_keep isl_id_to_ast_expr *NewAccesses

    ) {
  int VectorWidth = getVectorWidth();
  const Value *Pointer = Load->getPointerOperand();
  VectorType *VectorType = VectorType::get(
      dyn_cast<PointerType>(Pointer->getType())->getElementType(), VectorWidth);

  Value *Vector = UndefValue::get(VectorType);

  for (int i = 0; i < VectorWidth; i++) {
    Value *NewPointer =
        generateLocationAccessed(Stmt, Load, Pointer, ScalarMaps[i],
                                 GlobalMaps[i], VLTS[i], NewAccesses);
    Value *ScalarLoad =
        Builder.CreateLoad(NewPointer, Load->getName() + "_p_scalar_");
    Vector = Builder.CreateInsertElement(
        Vector, ScalarLoad, Builder.getInt32(i), Load->getName() + "_p_vec_");
  }

  return Vector;
}

void VectorBlockGenerator::generateLoad(
    ScopStmt &Stmt, const LoadInst *Load, ValueMapT &VectorMap,
    VectorValueMapT &ScalarMaps, __isl_keep isl_id_to_ast_expr *NewAccesses) {
  if (!VectorType::isValidElementType(Load->getType())) {
    for (int i = 0; i < getVectorWidth(); i++)
      ScalarMaps[i][Load] = generateScalarLoad(
          Stmt, Load, ScalarMaps[i], GlobalMaps[i], VLTS[i], NewAccesses);
    return;
  }

  const MemoryAccess &Access = Stmt.getAccessFor(Load);

  // Make sure we have scalar values available to access the pointer to
  // the data location.
  extractScalarValues(Load, VectorMap, ScalarMaps);

  Value *NewLoad;
  if (Access.isStrideZero(isl_map_copy(Schedule)))
    NewLoad = generateStrideZeroLoad(Stmt, Load, ScalarMaps[0], NewAccesses);
  else if (Access.isStrideOne(isl_map_copy(Schedule)))
    NewLoad = generateStrideOneLoad(Stmt, Load, ScalarMaps, NewAccesses);
  else if (Access.isStrideX(isl_map_copy(Schedule), -1))
    NewLoad = generateStrideOneLoad(Stmt, Load, ScalarMaps, NewAccesses, true);
  else
    NewLoad = generateUnknownStrideLoad(Stmt, Load, ScalarMaps, NewAccesses);

  VectorMap[Load] = NewLoad;
}

void VectorBlockGenerator::copyUnaryInst(ScopStmt &Stmt,
                                         const UnaryInstruction *Inst,
                                         ValueMapT &VectorMap,
                                         VectorValueMapT &ScalarMaps) {
  int VectorWidth = getVectorWidth();
  Value *NewOperand = getVectorValue(Stmt, Inst->getOperand(0), VectorMap,
                                     ScalarMaps, getLoopForInst(Inst));

  assert(isa<CastInst>(Inst) && "Can not generate vector code for instruction");

  const CastInst *Cast = dyn_cast<CastInst>(Inst);
  VectorType *DestType = VectorType::get(Inst->getType(), VectorWidth);
  VectorMap[Inst] = Builder.CreateCast(Cast->getOpcode(), NewOperand, DestType);
}

void VectorBlockGenerator::copyBinaryInst(ScopStmt &Stmt,
                                          const BinaryOperator *Inst,
                                          ValueMapT &VectorMap,
                                          VectorValueMapT &ScalarMaps) {
  Loop *L = getLoopForInst(Inst);
  Value *OpZero = Inst->getOperand(0);
  Value *OpOne = Inst->getOperand(1);

  Value *NewOpZero, *NewOpOne;
  NewOpZero = getVectorValue(Stmt, OpZero, VectorMap, ScalarMaps, L);
  NewOpOne = getVectorValue(Stmt, OpOne, VectorMap, ScalarMaps, L);

  Value *NewInst = Builder.CreateBinOp(Inst->getOpcode(), NewOpZero, NewOpOne,
                                       Inst->getName() + "p_vec");
  VectorMap[Inst] = NewInst;
}

void VectorBlockGenerator::copyStore(
    ScopStmt &Stmt, const StoreInst *Store, ValueMapT &VectorMap,
    VectorValueMapT &ScalarMaps, __isl_keep isl_id_to_ast_expr *NewAccesses) {
  const MemoryAccess &Access = Stmt.getAccessFor(Store);

  const Value *Pointer = Store->getPointerOperand();
  Value *Vector = getVectorValue(Stmt, Store->getValueOperand(), VectorMap,
                                 ScalarMaps, getLoopForInst(Store));

  // Make sure we have scalar values available to access the pointer to
  // the data location.
  extractScalarValues(Store, VectorMap, ScalarMaps);

  if (Access.isStrideOne(isl_map_copy(Schedule))) {
    Type *VectorPtrType = getVectorPtrTy(Pointer, getVectorWidth());
    Value *NewPointer =
        generateLocationAccessed(Stmt, Store, Pointer, ScalarMaps[0],
                                 GlobalMaps[0], VLTS[0], NewAccesses);

    Value *VectorPtr =
        Builder.CreateBitCast(NewPointer, VectorPtrType, "vector_ptr");
    StoreInst *Store = Builder.CreateStore(Vector, VectorPtr);

    if (!Aligned)
      Store->setAlignment(8);
  } else {
    for (unsigned i = 0; i < ScalarMaps.size(); i++) {
      Value *Scalar = Builder.CreateExtractElement(Vector, Builder.getInt32(i));
      Value *NewPointer =
          generateLocationAccessed(Stmt, Store, Pointer, ScalarMaps[i],
                                   GlobalMaps[i], VLTS[i], NewAccesses);
      Builder.CreateStore(Scalar, NewPointer);
    }
  }
}

bool VectorBlockGenerator::hasVectorOperands(const Instruction *Inst,
                                             ValueMapT &VectorMap) {
  for (Value *Operand : Inst->operands())
    if (VectorMap.count(Operand))
      return true;
  return false;
}

bool VectorBlockGenerator::extractScalarValues(const Instruction *Inst,
                                               ValueMapT &VectorMap,
                                               VectorValueMapT &ScalarMaps) {
  bool HasVectorOperand = false;
  int VectorWidth = getVectorWidth();

  for (Value *Operand : Inst->operands()) {
    ValueMapT::iterator VecOp = VectorMap.find(Operand);

    if (VecOp == VectorMap.end())
      continue;

    HasVectorOperand = true;
    Value *NewVector = VecOp->second;

    for (int i = 0; i < VectorWidth; ++i) {
      ValueMapT &SM = ScalarMaps[i];

      // If there is one scalar extracted, all scalar elements should have
      // already been extracted by the code here. So no need to check for the
      // existance of all of them.
      if (SM.count(Operand))
        break;

      SM[Operand] =
          Builder.CreateExtractElement(NewVector, Builder.getInt32(i));
    }
  }

  return HasVectorOperand;
}

void VectorBlockGenerator::copyInstScalarized(
    ScopStmt &Stmt, const Instruction *Inst, ValueMapT &VectorMap,
    VectorValueMapT &ScalarMaps, __isl_keep isl_id_to_ast_expr *NewAccesses) {
  bool HasVectorOperand;
  int VectorWidth = getVectorWidth();

  HasVectorOperand = extractScalarValues(Inst, VectorMap, ScalarMaps);

  for (int VectorLane = 0; VectorLane < getVectorWidth(); VectorLane++)
    BlockGenerator::copyInstruction(Stmt, Inst, ScalarMaps[VectorLane],
                                    GlobalMaps[VectorLane], VLTS[VectorLane],
                                    NewAccesses);

  if (!VectorType::isValidElementType(Inst->getType()) || !HasVectorOperand)
    return;

  // Make the result available as vector value.
  VectorType *VectorType = VectorType::get(Inst->getType(), VectorWidth);
  Value *Vector = UndefValue::get(VectorType);

  for (int i = 0; i < VectorWidth; i++)
    Vector = Builder.CreateInsertElement(Vector, ScalarMaps[i][Inst],
                                         Builder.getInt32(i));

  VectorMap[Inst] = Vector;
}

int VectorBlockGenerator::getVectorWidth() { return GlobalMaps.size(); }

void VectorBlockGenerator::copyInstruction(
    ScopStmt &Stmt, const Instruction *Inst, ValueMapT &VectorMap,
    VectorValueMapT &ScalarMaps, __isl_keep isl_id_to_ast_expr *NewAccesses) {
  // Terminator instructions control the control flow. They are explicitly
  // expressed in the clast and do not need to be copied.
  if (Inst->isTerminator())
    return;

  if (canSynthesize(Inst, &LI, &SE, &Stmt.getParent()->getRegion()))
    return;

  if (const LoadInst *Load = dyn_cast<LoadInst>(Inst)) {
    generateLoad(Stmt, Load, VectorMap, ScalarMaps, NewAccesses);
    return;
  }

  if (hasVectorOperands(Inst, VectorMap)) {
    if (const StoreInst *Store = dyn_cast<StoreInst>(Inst)) {
      copyStore(Stmt, Store, VectorMap, ScalarMaps, NewAccesses);
      return;
    }

    if (const UnaryInstruction *Unary = dyn_cast<UnaryInstruction>(Inst)) {
      copyUnaryInst(Stmt, Unary, VectorMap, ScalarMaps);
      return;
    }

    if (const BinaryOperator *Binary = dyn_cast<BinaryOperator>(Inst)) {
      copyBinaryInst(Stmt, Binary, VectorMap, ScalarMaps);
      return;
    }

    // Falltrough: We generate scalar instructions, if we don't know how to
    // generate vector code.
  }

  copyInstScalarized(Stmt, Inst, VectorMap, ScalarMaps, NewAccesses);
}

void VectorBlockGenerator::copyStmt(
    ScopStmt &Stmt, __isl_keep isl_id_to_ast_expr *NewAccesses) {
  assert(Stmt.isBlockStmt() && "TODO: Only block statements can be copied by "
                               "the vector block generator");

  BasicBlock *BB = Stmt.getBasicBlock();
  BasicBlock *CopyBB =
      SplitBlock(Builder.GetInsertBlock(), Builder.GetInsertPoint(), &DT, &LI);
  CopyBB->setName("polly.stmt." + BB->getName());
  Builder.SetInsertPoint(CopyBB->begin());

  // Create two maps that store the mapping from the original instructions of
  // the old basic block to their copies in the new basic block. Those maps
  // are basic block local.
  //
  // As vector code generation is supported there is one map for scalar values
  // and one for vector values.
  //
  // In case we just do scalar code generation, the vectorMap is not used and
  // the scalarMap has just one dimension, which contains the mapping.
  //
  // In case vector code generation is done, an instruction may either appear
  // in the vector map once (as it is calculating >vectorwidth< values at a
  // time. Or (if the values are calculated using scalar operations), it
  // appears once in every dimension of the scalarMap.
  VectorValueMapT ScalarBlockMap(getVectorWidth());
  ValueMapT VectorBlockMap;

  for (Instruction &Inst : *BB)
    copyInstruction(Stmt, &Inst, VectorBlockMap, ScalarBlockMap, NewAccesses);
}

BasicBlock *RegionGenerator::repairDominance(BasicBlock *BB,
                                             BasicBlock *BBCopy) {

  BasicBlock *BBIDom = DT.getNode(BB)->getIDom()->getBlock();
  BasicBlock *BBCopyIDom = BlockMap.lookup(BBIDom);

  if (BBCopyIDom)
    DT.changeImmediateDominator(BBCopy, BBCopyIDom);

  return BBCopyIDom;
}

void RegionGenerator::copyStmt(ScopStmt &Stmt, ValueMapT &GlobalMap,
                               LoopToScevMapT &LTS,
                               isl_id_to_ast_expr *IdToAstExp) {
  assert(Stmt.isRegionStmt() &&
         "Only region statements can be copied by the region generator");

  // Forget all old mappings.
  BlockMap.clear();
  RegionMaps.clear();
  IncompletePHINodeMap.clear();

  // The region represented by the statement.
  Region *R = Stmt.getRegion();

  // Create a dedicated entry for the region where we can reload all demoted
  // inputs.
  BasicBlock *EntryBB = R->getEntry();
  BasicBlock *EntryBBCopy =
      SplitBlock(Builder.GetInsertBlock(), Builder.GetInsertPoint(), &DT, &LI);
  EntryBBCopy->setName("polly.stmt." + EntryBB->getName() + ".entry");
  Builder.SetInsertPoint(EntryBBCopy->begin());

  for (auto PI = pred_begin(EntryBB), PE = pred_end(EntryBB); PI != PE; ++PI)
    if (!R->contains(*PI))
      BlockMap[*PI] = EntryBBCopy;

  // Iterate over all blocks in the region in a breadth-first search.
  std::deque<BasicBlock *> Blocks;
  SmallPtrSet<BasicBlock *, 8> SeenBlocks;
  Blocks.push_back(EntryBB);
  SeenBlocks.insert(EntryBB);

  while (!Blocks.empty()) {
    BasicBlock *BB = Blocks.front();
    Blocks.pop_front();

    // First split the block and update dominance information.
    BasicBlock *BBCopy = splitBB(BB);
    BasicBlock *BBCopyIDom = repairDominance(BB, BBCopy);

    // In order to remap PHI nodes we store also basic block mappings.
    BlockMap[BB] = BBCopy;

    // Get the mapping for this block and initialize it with the mapping
    // available at its immediate dominator (in the new region).
    ValueMapT &RegionMap = RegionMaps[BBCopy];
    RegionMap = RegionMaps[BBCopyIDom];

    // Copy the block with the BlockGenerator.
    copyBB(Stmt, BB, BBCopy, RegionMap, GlobalMap, LTS, IdToAstExp);

    // In order to remap PHI nodes we store also basic block mappings.
    BlockMap[BB] = BBCopy;

    // Add values to incomplete PHI nodes waiting for this block to be copied.
    for (const PHINodePairTy &PHINodePair : IncompletePHINodeMap[BB])
      addOperandToPHI(Stmt, PHINodePair.first, PHINodePair.second, BB,
                      GlobalMap, LTS);
    IncompletePHINodeMap[BB].clear();

    // And continue with new successors inside the region.
    for (auto SI = succ_begin(BB), SE = succ_end(BB); SI != SE; SI++)
      if (R->contains(*SI) && SeenBlocks.insert(*SI).second)
        Blocks.push_back(*SI);
  }

  // Now create a new dedicated region exit block and add it to the region map.
  BasicBlock *ExitBBCopy =
      SplitBlock(Builder.GetInsertBlock(), Builder.GetInsertPoint(), &DT, &LI);
  ExitBBCopy->setName("polly.stmt." + R->getExit()->getName() + ".exit");
  BlockMap[R->getExit()] = ExitBBCopy;

  repairDominance(R->getExit(), ExitBBCopy);

  // As the block generator doesn't handle control flow we need to add the
  // region control flow by hand after all blocks have been copied.
  for (BasicBlock *BB : SeenBlocks) {

    BranchInst *BI = cast<BranchInst>(BB->getTerminator());

    BasicBlock *BBCopy = BlockMap[BB];
    Instruction *BICopy = BBCopy->getTerminator();

    ValueMapT &RegionMap = RegionMaps[BBCopy];
    RegionMap.insert(BlockMap.begin(), BlockMap.end());

    Builder.SetInsertPoint(BICopy);
    copyInstScalar(Stmt, BI, RegionMap, GlobalMap, LTS);
    BICopy->eraseFromParent();
  }

  // Add counting PHI nodes to all loops in the region that can be used as
  // replacement for SCEVs refering to the old loop.
  for (BasicBlock *BB : SeenBlocks) {
    Loop *L = LI.getLoopFor(BB);
    if (L == nullptr || L->getHeader() != BB)
      continue;

    BasicBlock *BBCopy = BlockMap[BB];
    Value *NullVal = Builder.getInt32(0);
    PHINode *LoopPHI =
        PHINode::Create(Builder.getInt32Ty(), 2, "polly.subregion.iv");
    Instruction *LoopPHIInc = BinaryOperator::CreateAdd(
        LoopPHI, Builder.getInt32(1), "polly.subregion.iv.inc");
    LoopPHI->insertBefore(BBCopy->begin());
    LoopPHIInc->insertBefore(BBCopy->getTerminator());

    for (auto *PredBB : make_range(pred_begin(BB), pred_end(BB))) {
      if (!R->contains(PredBB))
        continue;
      if (L->contains(PredBB))
        LoopPHI->addIncoming(LoopPHIInc, BlockMap[PredBB]);
      else
        LoopPHI->addIncoming(NullVal, BlockMap[PredBB]);
    }

    for (auto *PredBBCopy : make_range(pred_begin(BBCopy), pred_end(BBCopy)))
      if (LoopPHI->getBasicBlockIndex(PredBBCopy) < 0)
        LoopPHI->addIncoming(NullVal, PredBBCopy);

    LTS[L] = SE.getUnknown(LoopPHI);
  }

  // Add all mappings from the region to the global map so outside uses will use
  // the copied instructions.
  for (auto &BBMap : RegionMaps)
    GlobalMap.insert(BBMap.second.begin(), BBMap.second.end());

  // Reset the old insert point for the build.
  Builder.SetInsertPoint(ExitBBCopy->begin());
}

void RegionGenerator::generateScalarLoads(ScopStmt &Stmt,
                                          const Instruction *Inst,
                                          ValueMapT &BBMap) {

  // Inside a non-affine region PHI nodes are copied not demoted. Once the
  // phi is copied it will reload all inputs from outside the region, hence
  // we do not need to generate code for the read access of the operands of a
  // PHI.
  if (isa<PHINode>(Inst))
    return;

  return BlockGenerator::generateScalarLoads(Stmt, Inst, BBMap);
}

void RegionGenerator::generateScalarStores(ScopStmt &Stmt, BasicBlock *BB,
                                           ValueMapT &BBMap,
                                           ValueMapT &GlobalMap) {
  const Region &R = Stmt.getParent()->getRegion();

  assert(Stmt.getRegion() &&
         "Block statements need to use the generateScalarStores() "
         "function in the BlockGenerator");

  for (MemoryAccess *MA : Stmt) {

    if (!MA->isScalar() || MA->isRead())
      continue;

    Instruction *ScalarInst = MA->getAccessInstruction();

    // Only generate accesses that belong to this basic block.
    if (ScalarInst->getParent() != BB)
      continue;

    Value *Val = MA->getAccessValue();

    auto Address = getOrCreateAlloca(*MA);

    Val = getNewScalarValue(Val, R, BBMap, GlobalMap);
    Builder.CreateStore(Val, Address);
  }
}

void RegionGenerator::addOperandToPHI(ScopStmt &Stmt, const PHINode *PHI,
                                      PHINode *PHICopy, BasicBlock *IncomingBB,
                                      ValueMapT &GlobalMap,
                                      LoopToScevMapT &LTS) {
  Region *StmtR = Stmt.getRegion();

  // If the incoming block was not yet copied mark this PHI as incomplete.
  // Once the block will be copied the incoming value will be added.
  BasicBlock *BBCopy = BlockMap[IncomingBB];
  if (!BBCopy) {
    assert(StmtR->contains(IncomingBB) &&
           "Bad incoming block for PHI in non-affine region");
    IncompletePHINodeMap[IncomingBB].push_back(std::make_pair(PHI, PHICopy));
    return;
  }

  Value *OpCopy = nullptr;
  if (StmtR->contains(IncomingBB)) {
    assert(RegionMaps.count(BBCopy) &&
           "Incoming PHI block did not have a BBMap");
    ValueMapT &BBCopyMap = RegionMaps[BBCopy];

    Value *Op = PHI->getIncomingValueForBlock(IncomingBB);
    OpCopy =
        getNewValue(Stmt, Op, BBCopyMap, GlobalMap, LTS, getLoopForInst(PHI));
  } else {

    if (PHICopy->getBasicBlockIndex(BBCopy) >= 0)
      return;

    AllocaInst *PHIOpAddr = getOrCreatePHIAlloca(const_cast<PHINode *>(PHI));
    OpCopy = new LoadInst(PHIOpAddr, PHIOpAddr->getName() + ".reload",
                          BlockMap[IncomingBB]->getTerminator());
  }

  assert(OpCopy && "Incoming PHI value was not copied properly");
  assert(BBCopy && "Incoming PHI block was not copied properly");
  PHICopy->addIncoming(OpCopy, BBCopy);
}

Value *RegionGenerator::copyPHIInstruction(ScopStmt &Stmt, const PHINode *PHI,
                                           ValueMapT &BBMap,
                                           ValueMapT &GlobalMap,
                                           LoopToScevMapT &LTS) {
  unsigned NumIncoming = PHI->getNumIncomingValues();
  PHINode *PHICopy =
      Builder.CreatePHI(PHI->getType(), NumIncoming, "polly." + PHI->getName());
  PHICopy->moveBefore(PHICopy->getParent()->getFirstNonPHI());
  BBMap[PHI] = PHICopy;

  for (unsigned u = 0; u < NumIncoming; u++)
    addOperandToPHI(Stmt, PHI, PHICopy, PHI->getIncomingBlock(u), GlobalMap,
                    LTS);
  return PHICopy;
}
