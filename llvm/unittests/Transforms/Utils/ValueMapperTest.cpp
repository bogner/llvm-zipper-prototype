//===- ValueMapper.cpp - Unit tests for ValueMapper -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

TEST(ValueMapperTest, MapMetadata) {
  LLVMContext Context;
  auto *U = MDTuple::get(Context, None);

  // The node should be unchanged.
  ValueToValueMapTy VM;
  EXPECT_EQ(U, MapMetadata(U, VM, RF_None));
}

TEST(ValueMapperTest, MapMetadataCycle) {
  LLVMContext Context;
  MDNode *U0;
  MDNode *U1;
  {
    Metadata *Ops[] = {nullptr};
    auto T = MDTuple::getTemporary(Context, Ops);
    Ops[0] = T.get();
    U0 = MDTuple::get(Context, Ops);
    T->replaceOperandWith(0, U0);
    U1 = MDNode::replaceWithUniqued(std::move(T));
    U0->resolveCycles();
  }

  EXPECT_TRUE(U0->isResolved());
  EXPECT_TRUE(U0->isUniqued());
  EXPECT_TRUE(U1->isResolved());
  EXPECT_TRUE(U1->isUniqued());
  EXPECT_EQ(U1, U0->getOperand(0));
  EXPECT_EQ(U0, U1->getOperand(0));

  // Cycles shouldn't be duplicated.
  {
    ValueToValueMapTy VM;
    EXPECT_EQ(U0, MapMetadata(U0, VM, RF_None));
    EXPECT_EQ(U1, MapMetadata(U1, VM, RF_None));
  }

  // Check the other order.
  {
    ValueToValueMapTy VM;
    EXPECT_EQ(U1, MapMetadata(U1, VM, RF_None));
    EXPECT_EQ(U0, MapMetadata(U0, VM, RF_None));
  }
}

TEST(ValueMapperTest, MapMetadataUnresolved) {
  LLVMContext Context;
  TempMDTuple T = MDTuple::getTemporary(Context, None);

  ValueToValueMapTy VM;
  EXPECT_EQ(T.get(), MapMetadata(T.get(), VM, RF_NoModuleLevelChanges));
}

TEST(ValueMapperTest, MapMetadataDistinct) {
  LLVMContext Context;
  auto *D = MDTuple::getDistinct(Context, None);

  {
    // The node should be cloned.
    ValueToValueMapTy VM;
    EXPECT_NE(D, MapMetadata(D, VM, RF_None));
  }
  {
    // The node should be moved.
    ValueToValueMapTy VM;
    EXPECT_EQ(D, MapMetadata(D, VM, RF_MoveDistinctMDs));
  }
}

TEST(ValueMapperTest, MapMetadataDistinctOperands) {
  LLVMContext Context;
  Metadata *Old = MDTuple::getDistinct(Context, None);
  auto *D = MDTuple::getDistinct(Context, Old);
  ASSERT_EQ(Old, D->getOperand(0));

  Metadata *New = MDTuple::getDistinct(Context, None);
  ValueToValueMapTy VM;
  VM.MD()[Old].reset(New);

  // Make sure operands are updated.
  EXPECT_EQ(D, MapMetadata(D, VM, RF_MoveDistinctMDs));
  EXPECT_EQ(New, D->getOperand(0));
}

TEST(ValueMapperTest, MapMetadataSeeded) {
  LLVMContext Context;
  auto *D = MDTuple::getDistinct(Context, None);

  // The node should be moved.
  ValueToValueMapTy VM;
  EXPECT_EQ(None, VM.getMappedMD(D));

  VM.MD().insert(std::make_pair(D, TrackingMDRef(D)));
  EXPECT_EQ(D, *VM.getMappedMD(D));
  EXPECT_EQ(D, MapMetadata(D, VM, RF_None));
}

TEST(ValueMapperTest, MapMetadataSeededWithNull) {
  LLVMContext Context;
  auto *D = MDTuple::getDistinct(Context, None);

  // The node should be moved.
  ValueToValueMapTy VM;
  EXPECT_EQ(None, VM.getMappedMD(D));

  VM.MD().insert(std::make_pair(D, TrackingMDRef()));
  EXPECT_EQ(nullptr, *VM.getMappedMD(D));
  EXPECT_EQ(nullptr, MapMetadata(D, VM, RF_None));
}

TEST(ValueMapperTest, MapMetadataLocalAsMetadata) {
  LLVMContext C;
  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(C), Type::getInt8Ty(C), false);
  std::unique_ptr<Function> F(
      Function::Create(FTy, GlobalValue::ExternalLinkage, "F"));
  Argument &A = *F->arg_begin();

  auto *LAM = LocalAsMetadata::get(&A);
  ValueToValueMapTy VM;
  EXPECT_EQ(nullptr, MapMetadata(LAM, VM));
  EXPECT_EQ(nullptr, MapMetadata(LAM, VM, RF_IgnoreMissingLocals));
  EXPECT_EQ(None, VM.getMappedMD(LAM));

  VM.MD()[LAM].reset(LAM);
  EXPECT_EQ(LAM, MapMetadata(LAM, VM));
  EXPECT_EQ(LAM, MapMetadata(LAM, VM, RF_IgnoreMissingLocals));

  auto *N = MDNode::get(C, None);
  VM.MD()[LAM].reset(N);
  EXPECT_EQ(N, MapMetadata(LAM, VM));
  EXPECT_EQ(N, MapMetadata(LAM, VM, RF_IgnoreMissingLocals));
}

TEST(ValueMapperTest, MapMetadataConstantAsMetadata) {
  LLVMContext C;
  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(C), Type::getInt8Ty(C), false);
  std::unique_ptr<Function> F(
      Function::Create(FTy, GlobalValue::ExternalLinkage, "F"));

  auto *CAM = ConstantAsMetadata::get(F.get());
  {
    ValueToValueMapTy VM;
    EXPECT_EQ(CAM, MapMetadata(CAM, VM));
    EXPECT_TRUE(VM.MD().count(CAM));
    VM.MD().erase(CAM);
    EXPECT_EQ(CAM, MapMetadata(CAM, VM, RF_IgnoreMissingLocals));
    EXPECT_TRUE(VM.MD().count(CAM));

    auto *N = MDNode::get(C, None);
    VM.MD()[CAM].reset(N);
    EXPECT_EQ(N, MapMetadata(CAM, VM));
    EXPECT_EQ(N, MapMetadata(CAM, VM, RF_IgnoreMissingLocals));
  }

  std::unique_ptr<Function> F2(
      Function::Create(FTy, GlobalValue::ExternalLinkage, "F2"));
  ValueToValueMapTy VM;
  VM[F.get()] = F2.get();
  auto *F2MD = MapMetadata(CAM, VM);
  EXPECT_TRUE(VM.MD().count(CAM));
  EXPECT_TRUE(F2MD);
  EXPECT_EQ(F2.get(), cast<ConstantAsMetadata>(F2MD)->getValue());
}

TEST(ValueMapperTest, MapValueLocalAsMetadata) {
  LLVMContext C;
  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(C), Type::getInt8Ty(C), false);
  std::unique_ptr<Function> F(
      Function::Create(FTy, GlobalValue::ExternalLinkage, "F"));
  Argument &A = *F->arg_begin();

  auto *LAM = LocalAsMetadata::get(&A);
  auto *MAV = MetadataAsValue::get(C, LAM);

  ValueToValueMapTy VM;
  EXPECT_EQ(nullptr, MapValue(MAV, VM));
  EXPECT_EQ(nullptr, MapValue(MAV, VM, RF_IgnoreMissingLocals));
  EXPECT_FALSE(VM.count(MAV));
  EXPECT_FALSE(VM.count(&A));

  VM[MAV] = MAV;
  EXPECT_EQ(MAV, MapValue(MAV, VM));
  EXPECT_EQ(MAV, MapValue(MAV, VM, RF_IgnoreMissingLocals));

  VM[MAV] = &A;
  EXPECT_EQ(&A, MapValue(MAV, VM));
  EXPECT_EQ(&A, MapValue(MAV, VM, RF_IgnoreMissingLocals));
}

TEST(ValueMapperTest, MapMetadataNullMapGlobalWithIgnoreMissingLocals) {
  LLVMContext C;
  FunctionType *FTy =
      FunctionType::get(Type::getVoidTy(C), Type::getInt8Ty(C), false);
  std::unique_ptr<Function> F(
      Function::Create(FTy, GlobalValue::ExternalLinkage, "F"));

  ValueToValueMapTy VM;
  RemapFlags Flags = RF_IgnoreMissingLocals | RF_NullMapMissingGlobalValues;
  EXPECT_EQ(nullptr, MapValue(F.get(), VM, Flags));
}

} // end namespace
