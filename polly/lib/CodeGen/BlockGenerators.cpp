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
#include "llvm/Analysis/ScalarEvolutionExpander.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "isl/aff.h"
#include "isl/ast.h"
#include "isl/set.h"
#include "isl/ast_build.h"

#include <deque>

using namespace llvm;
using namespace polly;

static cl::opt<bool> Aligned("enable-polly-aligned",
                             cl::desc("Assumed aligned memory accesses."),
                             cl::Hidden, cl::init(false), cl::ZeroOrMore,
                             cl::cat(PollyCategory));

bool polly::canSynthesize(const Instruction *I, const llvm::LoopInfo *LI,
                          ScalarEvolution *SE, const Region *R) {
  if (!I || !SE->isSCEVable(I->getType()))
    return false;

  if (const SCEV *Scev = SE->getSCEV(const_cast<Instruction *>(I)))
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
      return true;
    default:
      break;
    }
  }
  return false;
}

BlockGenerator::BlockGenerator(PollyIRBuilder &B, LoopInfo &LI,
                               ScalarEvolution &SE, DominatorTree &DT,
                               IslExprBuilder *ExprBuilder)
    : Builder(B), LI(LI), SE(SE), ExprBuilder(ExprBuilder), DT(DT) {}

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
        SCEVExpander Expander(SE, Stmt.getParent()
                                      ->getRegion()
                                      .getEntry()
                                      ->getParent()
                                      ->getParent()
                                      ->getDataLayout(),
                              "polly");
        Value *Expanded = Expander.expandCodeFor(NewScev, Old->getType(),
                                                 Builder.GetInsertPoint());

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

Value *BlockGenerator::getNewAccessOperand(ScopStmt &Stmt,
                                           const MemoryAccess &MA) {
  isl_pw_multi_aff *PWAccRel;
  isl_union_map *Schedule;
  isl_ast_expr *Expr;
  isl_ast_build *Build = Stmt.getAstBuild();

  assert(ExprBuilder && Build &&
         "Cannot generate new value without IslExprBuilder!");

  Schedule = isl_ast_build_get_schedule(Build);
  PWAccRel = MA.applyScheduleToAccessRelation(Schedule);

  Expr = isl_ast_build_access_from_pw_multi_aff(Build, PWAccRel);
  Expr = isl_ast_expr_address_of(Expr);

  return ExprBuilder->create(Expr);
}

Value *BlockGenerator::generateLocationAccessed(
    ScopStmt &Stmt, const Instruction *Inst, const Value *Pointer,
    ValueMapT &BBMap, ValueMapT &GlobalMap, LoopToScevMapT &LTS) {
  const MemoryAccess &MA = Stmt.getAccessFor(Inst);

  Value *NewPointer;
  if (MA.hasNewAccessRelation())
    NewPointer = getNewAccessOperand(Stmt, MA);
  else
    NewPointer =
        getNewValue(Stmt, Pointer, BBMap, GlobalMap, LTS, getLoopForInst(Inst));

  return NewPointer;
}

Loop *BlockGenerator::getLoopForInst(const llvm::Instruction *Inst) {
  return LI.getLoopFor(Inst->getParent());
}

Value *BlockGenerator::generateScalarLoad(ScopStmt &Stmt, const LoadInst *Load,
                                          ValueMapT &BBMap,
                                          ValueMapT &GlobalMap,
                                          LoopToScevMapT &LTS) {
  const Value *Pointer = Load->getPointerOperand();
  Value *NewPointer =
      generateLocationAccessed(Stmt, Load, Pointer, BBMap, GlobalMap, LTS);
  Value *ScalarLoad = Builder.CreateAlignedLoad(
      NewPointer, Load->getAlignment(), Load->getName() + "_p_scalar_");
  return ScalarLoad;
}

Value *BlockGenerator::generateScalarStore(ScopStmt &Stmt,
                                           const StoreInst *Store,
                                           ValueMapT &BBMap,
                                           ValueMapT &GlobalMap,
                                           LoopToScevMapT &LTS) {
  const Value *Pointer = Store->getPointerOperand();
  Value *NewPointer =
      generateLocationAccessed(Stmt, Store, Pointer, BBMap, GlobalMap, LTS);
  Value *ValueOperand = getNewValue(Stmt, Store->getValueOperand(), BBMap,
                                    GlobalMap, LTS, getLoopForInst(Store));

  Value *NewStore = Builder.CreateAlignedStore(ValueOperand, NewPointer,
                                               Store->getAlignment());
  return NewStore;
}

void BlockGenerator::copyInstruction(ScopStmt &Stmt, const Instruction *Inst,
                                     ValueMapT &BBMap, ValueMapT &GlobalMap,
                                     LoopToScevMapT &LTS) {
  // Terminator instructions control the control flow. They are explicitly
  // expressed in the clast and do not need to be copied.
  if (Inst->isTerminator())
    return;

  if (canSynthesize(Inst, &LI, &SE, &Stmt.getParent()->getRegion()))
    return;

  if (const LoadInst *Load = dyn_cast<LoadInst>(Inst)) {
    Value *NewLoad = generateScalarLoad(Stmt, Load, BBMap, GlobalMap, LTS);
    // Compute NewLoad before its insertion in BBMap to make the insertion
    // deterministic.
    BBMap[Load] = NewLoad;
    return;
  }

  if (const StoreInst *Store = dyn_cast<StoreInst>(Inst)) {
    Value *NewStore = generateScalarStore(Stmt, Store, BBMap, GlobalMap, LTS);
    // Compute NewStore before its insertion in BBMap to make the insertion
    // deterministic.
    BBMap[Store] = NewStore;
    return;
  }

  // Skip some special intrinsics for which we do not adjust the semantics to
  // the new schedule. All others are handled like every other instruction.
  if (auto *IT = dyn_cast<IntrinsicInst>(Inst)) {
    switch (IT->getIntrinsicID()) {
    // Lifetime markers are ignored.
    case llvm::Intrinsic::lifetime_start:
    case llvm::Intrinsic::lifetime_end:
    // Invariant markers are ignored.
    case llvm::Intrinsic::invariant_start:
    case llvm::Intrinsic::invariant_end:
    // Some misc annotations are ignored.
    case llvm::Intrinsic::var_annotation:
    case llvm::Intrinsic::ptr_annotation:
    case llvm::Intrinsic::annotation:
    case llvm::Intrinsic::donothing:
    case llvm::Intrinsic::assume:
    case llvm::Intrinsic::expect:
      return;
    default:
      // Other intrinsics are copied.
      break;
    }
  }

  copyInstScalar(Stmt, Inst, BBMap, GlobalMap, LTS);
}

void BlockGenerator::copyStmt(ScopStmt &Stmt, ValueMapT &GlobalMap,
                              LoopToScevMapT &LTS) {
  assert(Stmt.isBlockStmt() &&
         "Only block statements can be copied by the block generator");

  ValueMapT BBMap;

  BasicBlock *BB = Stmt.getBasicBlock();
  copyBB(Stmt, BB, BBMap, GlobalMap, LTS);
}

BasicBlock *BlockGenerator::splitBB(BasicBlock *BB) {
  BasicBlock *CopyBB =
      SplitBlock(Builder.GetInsertBlock(), Builder.GetInsertPoint(), &DT, &LI);
  CopyBB->setName("polly.stmt." + BB->getName());
  return CopyBB;
}

BasicBlock *BlockGenerator::copyBB(ScopStmt &Stmt, BasicBlock *BB,
                                   ValueMapT &BBMap, ValueMapT &GlobalMap,
                                   LoopToScevMapT &LTS) {
  BasicBlock *CopyBB = splitBB(BB);
  copyBB(Stmt, BB, CopyBB, BBMap, GlobalMap, LTS);
  return CopyBB;
}

void BlockGenerator::copyBB(ScopStmt &Stmt, BasicBlock *BB, BasicBlock *CopyBB,
                            ValueMapT &BBMap, ValueMapT &GlobalMap,
                            LoopToScevMapT &LTS) {
  Builder.SetInsertPoint(CopyBB->begin());
  for (Instruction &Inst : *BB)
    copyInstruction(Stmt, &Inst, BBMap, GlobalMap, LTS);
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
    bool NegativeStride = false) {
  unsigned VectorWidth = getVectorWidth();
  const Value *Pointer = Load->getPointerOperand();
  Type *VectorPtrType = getVectorPtrTy(Pointer, VectorWidth);
  unsigned Offset = NegativeStride ? VectorWidth - 1 : 0;

  Value *NewPointer = nullptr;
  NewPointer = generateLocationAccessed(Stmt, Load, Pointer, ScalarMaps[Offset],
                                        GlobalMaps[Offset], VLTS[Offset]);
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

Value *VectorBlockGenerator::generateStrideZeroLoad(ScopStmt &Stmt,
                                                    const LoadInst *Load,
                                                    ValueMapT &BBMap) {
  const Value *Pointer = Load->getPointerOperand();
  Type *VectorPtrType = getVectorPtrTy(Pointer, 1);
  Value *NewPointer = generateLocationAccessed(Stmt, Load, Pointer, BBMap,
                                               GlobalMaps[0], VLTS[0]);
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
    ScopStmt &Stmt, const LoadInst *Load, VectorValueMapT &ScalarMaps) {
  int VectorWidth = getVectorWidth();
  const Value *Pointer = Load->getPointerOperand();
  VectorType *VectorType = VectorType::get(
      dyn_cast<PointerType>(Pointer->getType())->getElementType(), VectorWidth);

  Value *Vector = UndefValue::get(VectorType);

  for (int i = 0; i < VectorWidth; i++) {
    Value *NewPointer = generateLocationAccessed(
        Stmt, Load, Pointer, ScalarMaps[i], GlobalMaps[i], VLTS[i]);
    Value *ScalarLoad =
        Builder.CreateLoad(NewPointer, Load->getName() + "_p_scalar_");
    Vector = Builder.CreateInsertElement(
        Vector, ScalarLoad, Builder.getInt32(i), Load->getName() + "_p_vec_");
  }

  return Vector;
}

void VectorBlockGenerator::generateLoad(ScopStmt &Stmt, const LoadInst *Load,
                                        ValueMapT &VectorMap,
                                        VectorValueMapT &ScalarMaps) {
  if (PollyVectorizerChoice == VECTORIZER_UNROLL_ONLY ||
      !VectorType::isValidElementType(Load->getType())) {
    for (int i = 0; i < getVectorWidth(); i++)
      ScalarMaps[i][Load] =
          generateScalarLoad(Stmt, Load, ScalarMaps[i], GlobalMaps[i], VLTS[i]);
    return;
  }

  const MemoryAccess &Access = Stmt.getAccessFor(Load);

  // Make sure we have scalar values available to access the pointer to
  // the data location.
  extractScalarValues(Load, VectorMap, ScalarMaps);

  Value *NewLoad;
  if (Access.isStrideZero(isl_map_copy(Schedule)))
    NewLoad = generateStrideZeroLoad(Stmt, Load, ScalarMaps[0]);
  else if (Access.isStrideOne(isl_map_copy(Schedule)))
    NewLoad = generateStrideOneLoad(Stmt, Load, ScalarMaps);
  else if (Access.isStrideX(isl_map_copy(Schedule), -1))
    NewLoad = generateStrideOneLoad(Stmt, Load, ScalarMaps, true);
  else
    NewLoad = generateUnknownStrideLoad(Stmt, Load, ScalarMaps);

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

void VectorBlockGenerator::copyStore(ScopStmt &Stmt, const StoreInst *Store,
                                     ValueMapT &VectorMap,
                                     VectorValueMapT &ScalarMaps) {
  const MemoryAccess &Access = Stmt.getAccessFor(Store);

  const Value *Pointer = Store->getPointerOperand();
  Value *Vector = getVectorValue(Stmt, Store->getValueOperand(), VectorMap,
                                 ScalarMaps, getLoopForInst(Store));

  // Make sure we have scalar values available to access the pointer to
  // the data location.
  extractScalarValues(Store, VectorMap, ScalarMaps);

  if (Access.isStrideOne(isl_map_copy(Schedule))) {
    Type *VectorPtrType = getVectorPtrTy(Pointer, getVectorWidth());
    Value *NewPointer = generateLocationAccessed(
        Stmt, Store, Pointer, ScalarMaps[0], GlobalMaps[0], VLTS[0]);

    Value *VectorPtr =
        Builder.CreateBitCast(NewPointer, VectorPtrType, "vector_ptr");
    StoreInst *Store = Builder.CreateStore(Vector, VectorPtr);

    if (!Aligned)
      Store->setAlignment(8);
  } else {
    for (unsigned i = 0; i < ScalarMaps.size(); i++) {
      Value *Scalar = Builder.CreateExtractElement(Vector, Builder.getInt32(i));
      Value *NewPointer = generateLocationAccessed(
          Stmt, Store, Pointer, ScalarMaps[i], GlobalMaps[i], VLTS[i]);
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

void VectorBlockGenerator::copyInstScalarized(ScopStmt &Stmt,
                                              const Instruction *Inst,
                                              ValueMapT &VectorMap,
                                              VectorValueMapT &ScalarMaps) {
  bool HasVectorOperand;
  int VectorWidth = getVectorWidth();

  HasVectorOperand = extractScalarValues(Inst, VectorMap, ScalarMaps);

  for (int VectorLane = 0; VectorLane < getVectorWidth(); VectorLane++)
    BlockGenerator::copyInstruction(Stmt, Inst, ScalarMaps[VectorLane],
                                    GlobalMaps[VectorLane], VLTS[VectorLane]);

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

void VectorBlockGenerator::copyInstruction(ScopStmt &Stmt,
                                           const Instruction *Inst,
                                           ValueMapT &VectorMap,
                                           VectorValueMapT &ScalarMaps) {
  // Terminator instructions control the control flow. They are explicitly
  // expressed in the clast and do not need to be copied.
  if (Inst->isTerminator())
    return;

  if (canSynthesize(Inst, &LI, &SE, &Stmt.getParent()->getRegion()))
    return;

  if (const LoadInst *Load = dyn_cast<LoadInst>(Inst)) {
    generateLoad(Stmt, Load, VectorMap, ScalarMaps);
    return;
  }

  if (hasVectorOperands(Inst, VectorMap)) {
    if (const StoreInst *Store = dyn_cast<StoreInst>(Inst)) {
      copyStore(Stmt, Store, VectorMap, ScalarMaps);
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

  copyInstScalarized(Stmt, Inst, VectorMap, ScalarMaps);
}

void VectorBlockGenerator::copyStmt(ScopStmt &Stmt) {
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
    copyInstruction(Stmt, &Inst, VectorBlockMap, ScalarBlockMap);
}

BasicBlock *RegionGenerator::repairDominance(
    BasicBlock *BB, BasicBlock *BBCopy,
    DenseMap<BasicBlock *, BasicBlock *> &BlockMap) {

  BasicBlock *BBIDom = DT.getNode(BB)->getIDom()->getBlock();
  BasicBlock *BBCopyIDom = BlockMap.lookup(BBIDom);

  if (BBCopyIDom)
    DT.changeImmediateDominator(BBCopy, BBCopyIDom);

  return BBCopyIDom;
}

void RegionGenerator::copyStmt(ScopStmt &Stmt, ValueMapT &GlobalMap,
                               LoopToScevMapT &LTS) {
  assert(Stmt.isRegionStmt() &&
         "Only region statements can be copied by the block generator");

  // The region represented by the statement.
  Region *R = Stmt.getRegion();

  // The "BBMaps" for the whole region.
  DenseMap<BasicBlock *, ValueMapT> RegionMaps;

  // A map from old to new blocks in the region
  DenseMap<BasicBlock *, BasicBlock *> BlockMap;

  // Iterate over all blocks in the region in a breadth-first search.
  std::deque<BasicBlock *> Blocks;
  SmallPtrSet<BasicBlock *, 8> SeenBlocks;
  Blocks.push_back(R->getEntry());
  SeenBlocks.insert(R->getEntry());

  while (!Blocks.empty()) {
    BasicBlock *BB = Blocks.front();
    Blocks.pop_front();

    // First split the block and update dominance information.
    BasicBlock *BBCopy = splitBB(BB);
    BasicBlock *BBCopyIDom = repairDominance(BB, BBCopy, BlockMap);

    // Get the mapping for this block and initialize it with the mapping
    // available at its immediate dominator (in the new region).
    ValueMapT &RegionMap = RegionMaps[BBCopy];
    RegionMap = RegionMaps[BBCopyIDom];

    // Copy the block with the BlockGenerator.
    copyBB(Stmt, BB, BBCopy, RegionMap, GlobalMap, LTS);

    // And continue with new successors inside the region.
    for (auto SI = succ_begin(BB), SE = succ_end(BB); SI != SE; SI++)
      if (R->contains(*SI) && SeenBlocks.insert(*SI).second)
        Blocks.push_back(*SI);

    // In order to remap PHI nodes we store also basic block mappings.
    BlockMap[BB] = BBCopy;
  }

  // Now create a new dedicated region exit block and add it to the region map.
  BasicBlock *ExitBBCopy =
      SplitBlock(Builder.GetInsertBlock(), Builder.GetInsertPoint(), &DT, &LI);
  ExitBBCopy->setName("polly.stmt." + R->getExit()->getName() + ".as.exit");
  BlockMap[R->getExit()] = ExitBBCopy;

  repairDominance(R->getExit(), ExitBBCopy, BlockMap);

  // As the block generator doesn't handle control flow we need to add the
  // region control flow by hand after all blocks have been copied.
  for (BasicBlock *BB : SeenBlocks) {

    BranchInst *BI = cast<BranchInst>(BB->getTerminator());

    BasicBlock *BBCopy = BlockMap[BB];
    Instruction *BICopy = BBCopy->getTerminator();

    ValueMapT &RegionMap = RegionMaps[BBCopy];
    RegionMap.insert(BlockMap.begin(), BlockMap.end());

    Builder.SetInsertPoint(BBCopy);
    copyInstScalar(Stmt, BI, RegionMap, GlobalMap, LTS);
    BICopy->eraseFromParent();
  }

  // Reset the old insert point for the build.
  Builder.SetInsertPoint(ExitBBCopy->begin());
}
