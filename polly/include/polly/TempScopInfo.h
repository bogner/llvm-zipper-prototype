//===-------- polly/TempScopInfo.h - Extract TempScops ----------*- C++ -*-===//
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

#ifndef POLLY_TEMP_SCOP_EXTRACTION_H
#define POLLY_TEMP_SCOP_EXTRACTION_H

#include "polly/ScopDetection.h"
#include "llvm/Analysis/RegionPass.h"
#include "llvm/IR/Instructions.h"

namespace llvm {
class DataLayout;
}

using namespace llvm;

namespace polly {

//===---------------------------------------------------------------------===//
/// @brief A memory access described by a SCEV expression and the access type.
class IRAccess {
public:
  Value *BaseAddress;
  Value *AccessValue;

  const SCEV *Offset;

  // The type of the scev affine function
  enum TypeKind {
    READ = 0x1,
    MUST_WRITE = 0x2,
    MAY_WRITE = 0x3,
  };

private:
  unsigned ElemBytes;
  TypeKind Type;
  bool IsAffine;

  /// @brief Is this IRAccess modeling special PHI node accesses?
  bool IsPHI;

public:
  SmallVector<const SCEV *, 4> Subscripts, Sizes;

  /// @brief Create a new IRAccess
  ///
  /// @param IsPHI Are we modeling special PHI node accesses?
  explicit IRAccess(TypeKind Type, Value *BaseAddress, const SCEV *Offset,
                    unsigned elemBytes, bool Affine, Value *AccessValue,
                    bool IsPHI = false)
      : BaseAddress(BaseAddress), AccessValue(AccessValue), Offset(Offset),
        ElemBytes(elemBytes), Type(Type), IsAffine(Affine), IsPHI(IsPHI) {}

  explicit IRAccess(TypeKind Type, Value *BaseAddress, const SCEV *Offset,
                    unsigned elemBytes, bool Affine,
                    SmallVector<const SCEV *, 4> Subscripts,
                    SmallVector<const SCEV *, 4> Sizes, Value *AccessValue)
      : BaseAddress(BaseAddress), AccessValue(AccessValue), Offset(Offset),
        ElemBytes(elemBytes), Type(Type), IsAffine(Affine), IsPHI(false),
        Subscripts(Subscripts), Sizes(Sizes) {}

  enum TypeKind getType() const { return Type; }

  Value *getBase() const { return BaseAddress; }

  Value *getAccessValue() const { return AccessValue; }

  const SCEV *getOffset() const { return Offset; }

  unsigned getElemSizeInBytes() const { return ElemBytes; }

  bool isAffine() const { return IsAffine; }

  bool isRead() const { return Type == READ; }

  bool isWrite() const { return Type == MUST_WRITE; }

  void setMayWrite() { Type = MAY_WRITE; }

  bool isMayWrite() const { return Type == MAY_WRITE; }

  bool isScalar() const { return Subscripts.size() == 0; }

  // @brief Is this IRAccess modeling special PHI node accesses?
  bool isPHI() const { return IsPHI; }

  void print(raw_ostream &OS) const;
};

class Comparison {
  const SCEV *LHS;
  const SCEV *RHS;

  ICmpInst::Predicate Pred;

public:
  Comparison(const SCEV *LHS, const SCEV *RHS, ICmpInst::Predicate Pred)
      : LHS(LHS), RHS(RHS), Pred(Pred) {}

  const SCEV *getLHS() const { return LHS; }
  const SCEV *getRHS() const { return RHS; }

  ICmpInst::Predicate getPred() const { return Pred; }
  void print(raw_ostream &OS) const;
};

//===---------------------------------------------------------------------===//

/// Maps from a loop to the affine function expressing its backedge taken count.
/// The backedge taken count already enough to express iteration domain as we
/// only allow loops with canonical induction variable.
/// A canonical induction variable is:
/// an integer recurrence that starts at 0 and increments by one each time
/// through the loop.
typedef std::map<const Loop *, const SCEV *> LoopBoundMapType;

typedef std::vector<std::pair<IRAccess, Instruction *>> AccFuncSetType;
typedef std::map<const BasicBlock *, AccFuncSetType> AccFuncMapType;

//===---------------------------------------------------------------------===//
/// @brief Scop represent with llvm objects.
///
/// A helper class for remembering the parameter number and the max depth in
/// this Scop, and others context.
class TempScop {
  // The Region.
  Region &R;

  // Access function of bbs.
  AccFuncMapType &AccFuncMap;

  friend class TempScopInfo;

  explicit TempScop(Region &r, AccFuncMapType &accFuncMap)
      : R(r), AccFuncMap(accFuncMap) {}

public:
  ~TempScop();

  /// @brief Get the maximum Region contained by this Scop.
  ///
  /// @return The maximum Region contained by this Scop.
  Region &getMaxRegion() const { return R; }

  /// @brief Get all access functions in a BasicBlock
  ///
  /// @param  BB The BasicBlock that containing the access functions.
  ///
  /// @return All access functions in BB
  ///
  AccFuncSetType *getAccessFunctions(const BasicBlock *BB) {
    AccFuncMapType::iterator at = AccFuncMap.find(BB);
    return at != AccFuncMap.end() ? &(at->second) : 0;
  }
  //@}

  /// @brief Print the Temporary Scop information.
  ///
  /// @param OS The output stream the access functions is printed to.
  /// @param SE The ScalarEvolution that help printing Temporary Scop
  ///           information.
  /// @param LI The LoopInfo that help printing the access functions.
  void print(raw_ostream &OS, ScalarEvolution *SE, LoopInfo *LI) const;

  /// @brief Print the access functions and loop bounds in this Scop.
  ///
  /// @param OS The output stream the access functions is printed to.
  /// @param SE The ScalarEvolution that help printing the access functions.
  /// @param LI The LoopInfo that help printing the access functions.
  void printDetail(raw_ostream &OS, ScalarEvolution *SE, LoopInfo *LI,
                   const Region *Reg, unsigned ind) const;
};

typedef std::map<const Region *, TempScop *> TempScopMapType;
//===----------------------------------------------------------------------===//
/// @brief The Function Pass to extract temporary information for Static control
///        part in llvm function.
///
class TempScopInfo : public RegionPass {
  //===-------------------------------------------------------------------===//
  TempScopInfo(const TempScopInfo &) = delete;
  const TempScopInfo &operator=(const TempScopInfo &) = delete;

  // The ScalarEvolution to help building Scop.
  ScalarEvolution *SE;

  // LoopInfo for information about loops
  LoopInfo *LI;

  // The AliasAnalysis to build AliasSetTracker.
  AliasAnalysis *AA;

  // Valid Regions for Scop
  ScopDetection *SD;

  // Target data for element size computing.
  const DataLayout *TD;

  // Access function of statements (currently BasicBlocks) .
  AccFuncMapType AccFuncMap;

  // Pre-created zero for the scalar accesses, with it we do not need create a
  // zero scev every time when we need it.
  const SCEV *ZeroOffset;

  // The TempScop for this region.
  TempScop *TempScopOfRegion;

  // Clear the context.
  void clear();

  // Build the temprory information of Region R, where R must be a valid part
  // of Scop.
  TempScop *buildTempScop(Region &R);

  /// @brief Build an instance of IRAccess from the Load/Store instruction.
  ///
  /// @param Inst       The Load/Store instruction that access the memory
  /// @param L          The parent loop of the instruction
  /// @param R          The region on which we are going to build a TempScop
  /// @param BoxedLoops The set of loops that are overapproximated in @p R.
  ///
  /// @return     The IRAccess to describe the access function of the
  ///             instruction.
  IRAccess buildIRAccess(Instruction *Inst, Loop *L, Region *R,
                         const ScopDetection::BoxedLoopsSetTy *BoxedLoops);

  /// @brief Analyze and extract the cross-BB scalar dependences (or,
  ///        dataflow dependencies) of an instruction.
  ///
  /// @param Inst               The instruction to be analyzed
  /// @param R                  The SCoP region
  /// @param NonAffineSubRegion The non affine sub-region @p Inst is in.
  ///
  /// @return     True if the Instruction is used in other BB and a scalar write
  ///             Access is required.
  bool buildScalarDependences(Instruction *Inst, Region *R,
                              Region *NonAffineSubRegio);

  /// @brief Create IRAccesses for the given PHI node in the given region.
  ///
  /// @param PHI                The PHI node to be handled
  /// @param R                  The SCoP region
  /// @param Functions          The access functions of the current BB
  /// @param NonAffineSubRegion The non affine sub-region @p PHI is in.
  /// @param IsExitBlock        Flag to indicate that @p PHI is in the exit BB.
  void buildPHIAccesses(PHINode *PHI, Region &R, AccFuncSetType &Functions,
                        Region *NonAffineSubRegion, bool IsExitBlock = false);

  /// @brief Build the access functions for the subregion @p SR.
  ///
  /// @param R  The SCoP region.
  /// @param SR A subregion of @p R.
  void buildAccessFunctions(Region &R, Region &SR);

  /// @brief Build the access functions for the basic block @p BB
  ///
  /// @param R                  The SCoP region.
  /// @param BB                 A basic block in @p R.
  /// @param NonAffineSubRegion The non affine sub-region @p BB is in.
  /// @param IsExitBlock        Flag to indicate that @p BB is in the exit BB.
  void buildAccessFunctions(Region &R, BasicBlock &BB,
                            Region *NonAffineSubRegion = nullptr,
                            bool IsExitBlock = false);

public:
  static char ID;
  explicit TempScopInfo() : RegionPass(ID), TempScopOfRegion(nullptr) {}
  ~TempScopInfo();

  /// @brief Get the temporay Scop information in LLVM IR for this region.
  ///
  /// @return The Scop information in LLVM IR represent.
  TempScop *getTempScop() const;

  /// @name RegionPass interface
  //@{
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual void releaseMemory() { clear(); }
  virtual bool runOnRegion(Region *R, RGPassManager &RGM);
  virtual void print(raw_ostream &OS, const Module *) const;
  //@}
};

} // end namespace polly

namespace llvm {
class PassRegistry;
void initializeTempScopInfoPass(llvm::PassRegistry &);
}

#endif
