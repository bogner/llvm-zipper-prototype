//===------ polly/ScopInfo.h - Create Scops from LLVM IR --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Create a polyhedral description for a static control flow region.
//
// The pass creates a polyhedral description of the Scops detected by the Scop
// detection derived from their LLVM-IR code.
//
// This representation is shared among several tools in the polyhedral
// community, which are e.g. CLooG, Pluto, Loopo, Graphite.
//
//===----------------------------------------------------------------------===//

#ifndef POLLY_SCOP_INFO_H
#define POLLY_SCOP_INFO_H

#include "polly/ScopDetection.h"
#include "polly/Support/SCEVAffinator.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/Analysis/RegionPass.h"
#include "isl/aff.h"
#include "isl/ctx.h"

#include <deque>
#include <forward_list>

using namespace llvm;

namespace llvm {
class AssumptionCache;
class Loop;
class LoopInfo;
class PHINode;
class ScalarEvolution;
class SCEV;
class SCEVAddRecExpr;
class Type;
}

struct isl_ctx;
struct isl_map;
struct isl_basic_map;
struct isl_id;
struct isl_set;
struct isl_union_set;
struct isl_union_map;
struct isl_space;
struct isl_ast_build;
struct isl_constraint;
struct isl_pw_aff;
struct isl_pw_multi_aff;
struct isl_schedule;

namespace polly {

class MemoryAccess;
class Scop;
class ScopStmt;
class ScopInfo;

//===---------------------------------------------------------------------===//

/// @brief Enumeration of assumptions Polly can take.
enum AssumptionKind {
  ALIASING,
  INBOUNDS,
  WRAPPING,
  ALIGNMENT,
  ERRORBLOCK,
  INFINITELOOP,
  INVARIANTLOAD,
  DELINEARIZATION,
};

/// Maps from a loop to the affine function expressing its backedge taken count.
/// The backedge taken count already enough to express iteration domain as we
/// only allow loops with canonical induction variable.
/// A canonical induction variable is:
/// an integer recurrence that starts at 0 and increments by one each time
/// through the loop.
typedef std::map<const Loop *, const SCEV *> LoopBoundMapType;

typedef std::deque<MemoryAccess> AccFuncSetType;
typedef std::map<const BasicBlock *, AccFuncSetType> AccFuncMapType;

/// @brief A class to store information about arrays in the SCoP.
///
/// Objects are accessible via the ScoP, MemoryAccess or the id associated with
/// the MemoryAccess access function.
///
class ScopArrayInfo {
public:
  /// @brief The kind of a ScopArrayInfo memory object.
  ///
  /// We distinguish between arrays and various scalar memory objects. We use
  /// the term ``array'' to describe memory objects that consist of a set of
  /// individual data elements arranged in a multi-dimensional grid. A scalar
  /// memory object describes an individual data element and is used to model
  /// the definition and uses of llvm::Values.
  ///
  /// The polyhedral model does traditionally not reason about SSA values. To
  /// reason about llvm::Values we model them "as if" they were zero-dimensional
  /// memory objects, even though they were not actually allocated in (main)
  /// memory.  Memory for such objects is only alloca[ed] at CodeGeneration
  /// time. To relate the memory slots used during code generation with the
  /// llvm::Values they belong to the new names for these corresponding stack
  /// slots are derived by appending suffixes (currently ".s2a" and ".phiops")
  /// to the name of the original llvm::Value. To describe how def/uses are
  /// modeled exactly we use these suffixes here as well.
  ///
  /// There are currently four different kinds of memory objects:
  enum MemoryKind {
    /// MK_Array: Models a one or multi-dimensional array
    ///
    /// A memory object that can be described by a multi-dimensional array.
    /// Memory objects of this type are used to model actual multi-dimensional
    /// arrays as they exist in LLVM-IR, but they are also used to describe
    /// other objects:
    ///   - A single data element allocated on the stack using 'alloca' is
    ///     modeled as a one-dimensional, single-element array.
    ///   - A single data element allocated as a global variable is modeled as
    ///     one-dimensional, single-element array.
    ///   - Certain multi-dimensional arrays with variable size, which in
    ///     LLVM-IR are commonly expressed as a single-dimensional access with a
    ///     complicated access function, are modeled as multi-dimensional
    ///     memory objects (grep for "delinearization").
    MK_Array,

    /// MK_Value: Models an llvm::Value
    ///
    /// Memory objects of type MK_Value are used to model the data flow
    /// induced by llvm::Values. For each llvm::Value that is used across
    /// BasicBocks one ScopArrayInfo object is created. A single memory WRITE
    /// stores the llvm::Value at its definition into the memory object and at
    /// each use of the llvm::Value (ignoring trivial intra-block uses) a
    /// corresponding READ is added. For instance, the use/def chain of a
    /// llvm::Value %V depicted below
    ///              ______________________
    ///              |DefBB:              |
    ///              |  %V = float op ... |
    ///              ----------------------
    ///               |                  |
    /// _________________               _________________
    /// |UseBB1:        |               |UseBB2:        |
    /// |  use float %V |               |  use float %V |
    /// -----------------               -----------------
    ///
    /// is modeled as if the following memory accesses occured:
    ///
    ///                        __________________________
    ///                        |entry:                  |
    ///                        |  %V.s2a = alloca float |
    ///                        --------------------------
    ///                                     |
    ///                    ___________________________________
    ///                    |DefBB:                           |
    ///                    |  store %float %V, float* %V.s2a |
    ///                    -----------------------------------
    ///                           |                   |
    /// ____________________________________ ___________________________________
    /// |UseBB1:                           | |UseBB2:                          |
    /// |  %V.reload1 = load float* %V.s2a | |  %V.reload2 = load float* %V.s2a|
    /// |  use float %V.reload1            | |  use float %V.reload2           |
    /// ------------------------------------ -----------------------------------
    ///
    MK_Value,

    /// MK_PHI: Models PHI nodes within the SCoP
    ///
    /// Besides the MK_Value memory object used to model the normal
    /// llvm::Value dependences described above, PHI nodes require an additional
    /// memory object of type MK_PHI to describe the forwarding of values to
    /// the PHI node.
    ///
    /// As an example, a PHIInst instructions
    ///
    /// %PHI = phi float [ %Val1, %IncomingBlock1 ], [ %Val2, %IncomingBlock2 ]
    ///
    /// is modeled as if the accesses occured this way:
    ///
    ///                    _______________________________
    ///                    |entry:                       |
    ///                    |  %PHI.phiops = alloca float |
    ///                    -------------------------------
    ///                           |              |
    /// __________________________________  __________________________________
    /// |IncomingBlock1:                 |  |IncomingBlock2:                 |
    /// |  ...                           |  |  ...                           |
    /// |  store float %Val1 %PHI.phiops |  |  store float %Val2 %PHI.phiops |
    /// |  br label % JoinBlock          |  |  br label %JoinBlock           |
    /// ----------------------------------  ----------------------------------
    ///                             \            /
    ///                              \          /
    ///               _________________________________________
    ///               |JoinBlock:                             |
    ///               |  %PHI = load float, float* PHI.phiops |
    ///               -----------------------------------------
    ///
    /// Note that there can also be a scalar write access for %PHI if used in a
    /// different BasicBlock, i.e. there can be a memory object %PHI.phiops as
    /// well as a memory object %PHI.s2a.
    MK_PHI,

    /// MK_ExitPHI: Models PHI nodes in the SCoP's exit block
    ///
    /// For PHI nodes in the Scop's exit block a special memory object kind is
    /// used. The modeling used is identical to MK_PHI, with the exception
    /// that there are no READs from these memory objects. The PHINode's
    /// llvm::Value is treated as a value escaping the SCoP. WRITE accesses
    /// write directly to the escaping value's ".s2a" alloca.
    MK_ExitPHI
  };

  /// @brief Construct a ScopArrayInfo object.
  ///
  /// @param BasePtr        The array base pointer.
  /// @param ElementType    The type of the elements stored in the array.
  /// @param IslCtx         The isl context used to create the base pointer id.
  /// @param DimensionSizes A vector containing the size of each dimension.
  /// @param Kind           The kind of the array object.
  /// @param DL             The data layout of the module.
  /// @param S              The scop this array object belongs to.
  ScopArrayInfo(Value *BasePtr, Type *ElementType, isl_ctx *IslCtx,
                ArrayRef<const SCEV *> DimensionSizes, enum MemoryKind Kind,
                const DataLayout &DL, Scop *S);

  ///  @brief Update the sizes of the ScopArrayInfo object.
  ///
  ///  A ScopArrayInfo object may with certain outer dimensions not being added
  ///  on the first creation. This function allows to update the sizes of the
  ///  ScopArrayInfo object by adding additional outer array dimensions.
  ///
  ///  @param A vector of array sizes where the rightmost array sizes need to
  ///         match the innermost array sizes already defined in SAI.
  ///  @returns Returns true if the update was successful, otherwise false.
  bool updateSizes(ArrayRef<const SCEV *> Sizes);

  /// @brief Destructor to free the isl id of the base pointer.
  ~ScopArrayInfo();

  /// @brief Set the base pointer to @p BP.
  void setBasePtr(Value *BP) { BasePtr = BP; }

  /// @brief Return the base pointer.
  Value *getBasePtr() const { return BasePtr; }

  /// @brief For indirect accesses return the origin SAI of the BP, else null.
  const ScopArrayInfo *getBasePtrOriginSAI() const { return BasePtrOriginSAI; }

  /// @brief The set of derived indirect SAIs for this origin SAI.
  const SmallPtrSetImpl<ScopArrayInfo *> &getDerivedSAIs() const {
    return DerivedSAIs;
  };

  /// @brief Return the number of dimensions.
  unsigned getNumberOfDimensions() const { return DimensionSizes.size(); }

  /// @brief Return the size of dimension @p dim as SCEV*.
  //
  //  Scalars do not have array dimensions and the first dimension of
  //  a (possibly multi-dimensional) array also does not carry any size
  //  information.
  const SCEV *getDimensionSize(unsigned Dim) const {
    assert(Dim > 0 && "Only dimensions larger than zero are sized.");
    assert(Dim < getNumberOfDimensions() && "Invalid dimension");
    return DimensionSizes[Dim - 1];
  }

  /// @brief Return the size of dimension @p dim as isl_pw_aff.
  //
  //  Scalars do not have array dimensions and the first dimension of
  //  a (possibly multi-dimensional) array also does not carry any size
  //  information.
  __isl_give isl_pw_aff *getDimensionSizePw(unsigned Dim) const {
    assert(Dim > 0 && "Only dimensions larger than zero are sized.");
    assert(Dim < getNumberOfDimensions() && "Invalid dimension");
    return isl_pw_aff_copy(DimensionSizesPw[Dim - 1]);
  }

  /// @brief Get the type of the elements stored in this array.
  Type *getElementType() const { return ElementType; }

  /// @brief Get element size in bytes.
  int getElemSizeInBytes() const;

  /// @brief Get the name of this memory reference.
  std::string getName() const;

  /// @brief Return the isl id for the base pointer.
  __isl_give isl_id *getBasePtrId() const;

  /// @brief Is this array info modeling special PHI node memory?
  ///
  /// During code generation of PHI nodes, there is a need for two kinds of
  /// virtual storage. The normal one as it is used for all scalar dependences,
  /// where the result of the PHI node is stored and later loaded from as well
  /// as a second one where the incoming values of the PHI nodes are stored
  /// into and reloaded when the PHI is executed. As both memories use the
  /// original PHI node as virtual base pointer, we have this additional
  /// attribute to distinguish the PHI node specific array modeling from the
  /// normal scalar array modeling.
  bool isPHIKind() const { return Kind == MK_PHI; };

  /// @brief Dump a readable representation to stderr.
  void dump() const;

  /// @brief Print a readable representation to @p OS.
  ///
  /// @param SizeAsPwAff Print the size as isl_pw_aff
  void print(raw_ostream &OS, bool SizeAsPwAff = false) const;

  /// @brief Access the ScopArrayInfo associated with an access function.
  static const ScopArrayInfo *
  getFromAccessFunction(__isl_keep isl_pw_multi_aff *PMA);

  /// @brief Access the ScopArrayInfo associated with an isl Id.
  static const ScopArrayInfo *getFromId(__isl_take isl_id *Id);

  /// @brief Get the space of this array access.
  __isl_give isl_space *getSpace() const;

private:
  void addDerivedSAI(ScopArrayInfo *DerivedSAI) {
    DerivedSAIs.insert(DerivedSAI);
  }

  /// @brief For indirect accesses this is the SAI of the BP origin.
  const ScopArrayInfo *BasePtrOriginSAI;

  /// @brief For origin SAIs the set of derived indirect SAIs.
  SmallPtrSet<ScopArrayInfo *, 2> DerivedSAIs;

  /// @brief The base pointer.
  AssertingVH<Value> BasePtr;

  /// @brief The type of the elements stored in this array.
  Type *ElementType;

  /// @brief The isl id for the base pointer.
  isl_id *Id;

  /// @brief The sizes of each dimension as SCEV*.
  SmallVector<const SCEV *, 4> DimensionSizes;

  /// @brief The sizes of each dimension as isl_pw_aff.
  SmallVector<isl_pw_aff *, 4> DimensionSizesPw;

  /// @brief The type of this scop array info object.
  ///
  /// We distinguish between SCALAR, PHI and ARRAY objects.
  enum MemoryKind Kind;

  /// @brief The data layout of the module.
  const DataLayout &DL;

  /// @brief The scop this SAI object belongs to.
  Scop &S;
};

/// @brief Represent memory accesses in statements.
class MemoryAccess {
  friend class Scop;
  friend class ScopStmt;

public:
  /// @brief The access type of a memory access
  ///
  /// There are three kind of access types:
  ///
  /// * A read access
  ///
  /// A certain set of memory locations are read and may be used for internal
  /// calculations.
  ///
  /// * A must-write access
  ///
  /// A certain set of memory locations is definitely written. The old value is
  /// replaced by a newly calculated value. The old value is not read or used at
  /// all.
  ///
  /// * A may-write access
  ///
  /// A certain set of memory locations may be written. The memory location may
  /// contain a new value if there is actually a write or the old value may
  /// remain, if no write happens.
  enum AccessType {
    READ = 0x1,
    MUST_WRITE = 0x2,
    MAY_WRITE = 0x3,
  };

  /// @brief Reduction access type
  ///
  /// Commutative and associative binary operations suitable for reductions
  enum ReductionType {
    RT_NONE, ///< Indicate no reduction at all
    RT_ADD,  ///< Addition
    RT_MUL,  ///< Multiplication
    RT_BOR,  ///< Bitwise Or
    RT_BXOR, ///< Bitwise XOr
    RT_BAND, ///< Bitwise And
  };

private:
  MemoryAccess(const MemoryAccess &) = delete;
  const MemoryAccess &operator=(const MemoryAccess &) = delete;

  /// @brief A unique identifier for this memory access.
  ///
  /// The identifier is unique between all memory accesses belonging to the same
  /// scop statement.
  isl_id *Id;

  /// @brief What is modeled by this MemoryAccess.
  /// @see ScopArrayInfo::MemoryKind
  ScopArrayInfo::MemoryKind Kind;

  /// @brief Whether it a reading or writing access, and if writing, whether it
  /// is conditional (MAY_WRITE).
  enum AccessType AccType;

  /// @brief Reduction type for reduction like accesses, RT_NONE otherwise
  ///
  /// An access is reduction like if it is part of a load-store chain in which
  /// both access the same memory location (use the same LLVM-IR value
  /// as pointer reference). Furthermore, between the load and the store there
  /// is exactly one binary operator which is known to be associative and
  /// commutative.
  ///
  /// TODO:
  ///
  /// We can later lift the constraint that the same LLVM-IR value defines the
  /// memory location to handle scops such as the following:
  ///
  ///    for i
  ///      for j
  ///        sum[i+j] = sum[i] + 3;
  ///
  /// Here not all iterations access the same memory location, but iterations
  /// for which j = 0 holds do. After lifting the equality check in ScopInfo,
  /// subsequent transformations do not only need check if a statement is
  /// reduction like, but they also need to verify that that the reduction
  /// property is only exploited for statement instances that load from and
  /// store to the same data location. Doing so at dependence analysis time
  /// could allow us to handle the above example.
  ReductionType RedType = RT_NONE;

  /// @brief Parent ScopStmt of this access.
  ScopStmt *Statement;

  // Properties describing the accessed array.
  // TODO: It might be possible to move them to ScopArrayInfo.
  // @{

  /// @brief The base address (e.g., A for A[i+j]).
  ///
  /// The #BaseAddr of a memory access of kind MK_Array is the base pointer
  /// of the memory access.
  /// The #BaseAddr of a memory access of kind MK_PHI or MK_ExitPHI is the
  /// PHI node itself.
  /// The #BaseAddr of a memory access of kind MK_Value is the instruction
  /// defining the value.
  AssertingVH<Value> BaseAddr;

  /// @brief An unique name of the accessed array.
  std::string BaseName;

  /// @brief Size in bytes of a single array element.
  unsigned ElemBytes;

  /// @brief Size of each dimension of the accessed array.
  SmallVector<const SCEV *, 4> Sizes;
  // @}

  // Properties describing the accessed element.
  // @{

  /// @brief The access instruction of this memory access.
  ///
  /// For memory accesses of kind MK_Array the access instruction is the
  /// Load or Store instruction performing the access.
  ///
  /// For memory accesses of kind MK_PHI or MK_ExitPHI the access
  /// instruction of a load access is the PHI instruction. The access
  /// instruction of a PHI-store is the incoming's block's terminator
  /// intruction.
  ///
  /// For memory accesses of kind MK_Value the access instruction of a load
  /// access is the instruction that uses the load. The access instruction of
  /// a write access is the instruction that defines the llvm::Value.
  Instruction *AccessInstruction;

  /// @brief The value associated with this memory access.
  ///
  ///  - For array memory accesses (MK_Array) it is the loaded result or the
  ///    stored value.
  ///  - For accesses of kind MK_Value it is the access instruction itself.
  ///  - For accesses of kind MK_PHI or MK_ExitPHI it is the operand value
  ///    of the PHI node.
  ///
  AssertingVH<Value> AccessValue;

  /// @brief Are all the subscripts affine expression?
  bool IsAffine;

  /// @brief Subscript expression for each dimension.
  SmallVector<const SCEV *, 4> Subscripts;

  /// @brief Relation from statment instances to the accessed array elements.
  isl_map *AccessRelation;

  /// @brief Updated access relation read from JSCOP file.
  isl_map *NewAccessRelation;
  // @}

  unsigned getElemSizeInBytes() const { return ElemBytes; }

  bool isAffine() const { return IsAffine; }

  __isl_give isl_basic_map *createBasicAccessMap(ScopStmt *Statement);

  void assumeNoOutOfBound();

  /// @brief Compute bounds on an over approximated  access relation.
  ///
  /// @param ElementSize The size of one element accessed.
  void computeBoundsOnAccessRelation(unsigned ElementSize);

  /// @brief Get the original access function as read from IR.
  __isl_give isl_map *getOriginalAccessRelation() const;

  /// @brief Return the space in which the access relation lives in.
  __isl_give isl_space *getOriginalAccessRelationSpace() const;

  /// @brief Get the new access function imported or set by a pass
  __isl_give isl_map *getNewAccessRelation() const;

  /// @brief Fold the memory access to consider parameteric offsets
  ///
  /// To recover memory accesses with array size parameters in the subscript
  /// expression we post-process the delinearization results.
  ///
  /// We would normally recover from an access A[exp0(i) * N + exp1(i)] into an
  /// array A[][N] the 2D access A[exp0(i)][exp1(i)]. However, another valid
  /// delinearization is A[exp0(i) - 1][exp1(i) + N] which - depending on the
  /// range of exp1(i) - may be preferrable. Specifically, for cases where we
  /// know exp1(i) is negative, we want to choose the latter expression.
  ///
  /// As we commonly do not have any information about the range of exp1(i),
  /// we do not choose one of the two options, but instead create a piecewise
  /// access function that adds the (-1, N) offsets as soon as exp1(i) becomes
  /// negative. For a 2D array such an access function is created by applying
  /// the piecewise map:
  ///
  /// [i,j] -> [i, j] :      j >= 0
  /// [i,j] -> [i-1, j+N] :  j <  0
  ///
  /// We can generalize this mapping to arbitrary dimensions by applying this
  /// piecewise mapping pairwise from the rightmost to the leftmost access
  /// dimension. It would also be possible to cover a wider range by introducing
  /// more cases and adding multiple of Ns to these cases. However, this has
  /// not yet been necessary.
  /// The introduction of different cases necessarily complicates the memory
  /// access function, but cases that can be statically proven to not happen
  /// will be eliminated later on.
  __isl_give isl_map *foldAccess(__isl_take isl_map *AccessRelation,
                                 ScopStmt *Statement);

  /// @brief Assemble the access relation from all availbale information.
  ///
  /// In particular, used the information passes in the constructor and the
  /// parent ScopStmt set by setStatment().
  ///
  /// @param SAI Info object for the accessed array.
  void buildAccessRelation(const ScopArrayInfo *SAI);

public:
  /// @brief Create a new MemoryAccess.
  ///
  /// @param Stmt       The parent statement.
  /// @param AccessInst The instruction doing the access.
  /// @param BaseAddr   The accessed array's address.
  /// @param ElemBytes  Number of accessed bytes.
  /// @param AccType    Whether read or write access.
  /// @param IsAffine   Whether the subscripts are affine expressions.
  /// @param Kind       The kind of memory accessed.
  /// @param Subscripts Subscipt expressions
  /// @param Sizes      Dimension lengths of the accessed array.
  /// @param BaseName   Name of the acessed array.
  MemoryAccess(ScopStmt *Stmt, Instruction *AccessInst, AccessType Type,
               Value *BaseAddress, unsigned ElemBytes, bool Affine,
               ArrayRef<const SCEV *> Subscripts, ArrayRef<const SCEV *> Sizes,
               Value *AccessValue, ScopArrayInfo::MemoryKind Kind,
               StringRef BaseName);
  ~MemoryAccess();

  /// @brief Get the type of a memory access.
  enum AccessType getType() { return AccType; }

  /// @brief Is this a reduction like access?
  bool isReductionLike() const { return RedType != RT_NONE; }

  /// @brief Is this a read memory access?
  bool isRead() const { return AccType == MemoryAccess::READ; }

  /// @brief Is this a must-write memory access?
  bool isMustWrite() const { return AccType == MemoryAccess::MUST_WRITE; }

  /// @brief Is this a may-write memory access?
  bool isMayWrite() const { return AccType == MemoryAccess::MAY_WRITE; }

  /// @brief Is this a write memory access?
  bool isWrite() const { return isMustWrite() || isMayWrite(); }

  /// @brief Check if a new access relation was imported or set by a pass.
  bool hasNewAccessRelation() const { return NewAccessRelation; }

  /// @brief Return the newest access relation of this access.
  ///
  /// There are two possibilities:
  ///   1) The original access relation read from the LLVM-IR.
  ///   2) A new access relation imported from a json file or set by another
  ///      pass (e.g., for privatization).
  ///
  /// As 2) is by construction "newer" than 1) we return the new access
  /// relation if present.
  ///
  isl_map *getAccessRelation() const {
    return hasNewAccessRelation() ? getNewAccessRelation()
                                  : getOriginalAccessRelation();
  }

  /// @brief Return the access relation after the schedule was applied.
  __isl_give isl_pw_multi_aff *
  applyScheduleToAccessRelation(__isl_take isl_union_map *Schedule) const;

  /// @brief Get an isl string representing the access function read from IR.
  std::string getOriginalAccessRelationStr() const;

  /// @brief Get an isl string representing a new access function, if available.
  std::string getNewAccessRelationStr() const;

  /// @brief Get the base address of this access (e.g. A for A[i+j]).
  Value *getBaseAddr() const { return BaseAddr; }

  /// @brief Get the base array isl_id for this access.
  __isl_give isl_id *getArrayId() const;

  /// @brief Get the ScopArrayInfo object for the base address.
  const ScopArrayInfo *getScopArrayInfo() const;

  /// @brief Return a string representation of the accesse's reduction type.
  const std::string getReductionOperatorStr() const;

  /// @brief Return a string representation of the reduction type @p RT.
  static const std::string getReductionOperatorStr(ReductionType RT);

  const std::string &getBaseName() const { return BaseName; }

  /// @brief Return the access value of this memory access.
  Value *getAccessValue() const { return AccessValue; }

  /// @brief Return the access instruction of this memory access.
  Instruction *getAccessInstruction() const { return AccessInstruction; }

  /// Get the stride of this memory access in the specified Schedule. Schedule
  /// is a map from the statement to a schedule where the innermost dimension is
  /// the dimension of the innermost loop containing the statement.
  __isl_give isl_set *getStride(__isl_take const isl_map *Schedule) const;

  /// Is the stride of the access equal to a certain width? Schedule is a map
  /// from the statement to a schedule where the innermost dimension is the
  /// dimension of the innermost loop containing the statement.
  bool isStrideX(__isl_take const isl_map *Schedule, int StrideWidth) const;

  /// Is consecutive memory accessed for a given statement instance set?
  /// Schedule is a map from the statement to a schedule where the innermost
  /// dimension is the dimension of the innermost loop containing the
  /// statement.
  bool isStrideOne(__isl_take const isl_map *Schedule) const;

  /// Is always the same memory accessed for a given statement instance set?
  /// Schedule is a map from the statement to a schedule where the innermost
  /// dimension is the dimension of the innermost loop containing the
  /// statement.
  bool isStrideZero(__isl_take const isl_map *Schedule) const;

  /// @brief Whether this is an access of an explicit load or store in the IR.
  bool isArrayKind() const { return Kind == ScopArrayInfo::MK_Array; }

  /// @brief Whether this access is an array to a scalar memory object.
  ///
  /// Scalar accesses are accesses to MK_Value, MK_PHI or MK_ExitPHI.
  bool isScalarKind() const { return !isArrayKind(); }

  /// @brief Is this MemoryAccess modeling scalar dependences?
  bool isValueKind() const { return Kind == ScopArrayInfo::MK_Value; }

  /// @brief Is this MemoryAccess modeling special PHI node accesses?
  bool isPHIKind() const { return Kind == ScopArrayInfo::MK_PHI; }

  /// @brief Is this MemoryAccess modeling the accesses of a PHI node in the
  /// SCoP's exit block?
  bool isExitPHIKind() const { return Kind == ScopArrayInfo::MK_ExitPHI; }

  /// @brief Get the statement that contains this memory access.
  ScopStmt *getStatement() const { return Statement; }

  /// @brief Get the reduction type of this access
  ReductionType getReductionType() const { return RedType; }

  /// @brief Set the updated access relation read from JSCOP file.
  void setNewAccessRelation(__isl_take isl_map *NewAccessRelation);

  /// @brief Mark this a reduction like access
  void markAsReductionLike(ReductionType RT) { RedType = RT; }

  /// @brief Align the parameters in the access relation to the scop context
  void realignParams();

  /// @brief Update the dimensionality of the memory access.
  ///
  /// During scop construction some memory accesses may not be constructed with
  /// their full dimensionality, but outer dimensions that may have been omitted
  /// if they took the value 'zero'. By updating the dimensionality of the
  /// statement we add additional zero-valued dimensions to match the
  /// dimensionality of the ScopArrayInfo object that belongs to this memory
  /// access.
  void updateDimensionality();

  /// @brief Get identifier for the memory access.
  ///
  /// This identifier is unique for all accesses that belong to the same scop
  /// statement.
  __isl_give isl_id *getId() const;

  /// @brief Print the MemoryAccess.
  ///
  /// @param OS The output stream the MemoryAccess is printed to.
  void print(raw_ostream &OS) const;

  /// @brief Print the MemoryAccess to stderr.
  void dump() const;
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              MemoryAccess::ReductionType RT);

/// @brief Ordered list type to hold accesses.
using MemoryAccessList = std::forward_list<MemoryAccess *>;

/// @brief Type for equivalent invariant accesses and their domain context.
///
/// The first element is the SCEV for the pointer/location that identifies this
/// equivalence class. The second is a list of memory accesses to that location
/// that are now treated as invariant and hoisted during code generation. The
/// last element is the execution context under which the invariant memory
/// location is accessed, hence the union of all domain contexts for the memory
/// accesses in the list.
using InvariantEquivClassTy =
    std::tuple<const SCEV *, MemoryAccessList, isl_set *>;

/// @brief Type for invariant accesses equivalence classes.
using InvariantEquivClassesTy = SmallVector<InvariantEquivClassTy, 8>;

///===----------------------------------------------------------------------===//
/// @brief Statement of the Scop
///
/// A Scop statement represents an instruction in the Scop.
///
/// It is further described by its iteration domain, its schedule and its data
/// accesses.
/// At the moment every statement represents a single basic block of LLVM-IR.
class ScopStmt {
public:
  ScopStmt(const ScopStmt &) = delete;
  const ScopStmt &operator=(const ScopStmt &) = delete;

  /// Create the ScopStmt from a BasicBlock.
  ScopStmt(Scop &parent, BasicBlock &bb);

  /// Create an overapproximating ScopStmt for the region @p R.
  ScopStmt(Scop &parent, Region &R);

  /// Initialize members after all MemoryAccesses have been added.
  void init();

private:
  /// Polyhedral description
  //@{

  /// The Scop containing this ScopStmt
  Scop &Parent;

  /// The iteration domain describes the set of iterations for which this
  /// statement is executed.
  ///
  /// Example:
  ///     for (i = 0; i < 100 + b; ++i)
  ///       for (j = 0; j < i; ++j)
  ///         S(i,j);
  ///
  /// 'S' is executed for different values of i and j. A vector of all
  /// induction variables around S (i, j) is called iteration vector.
  /// The domain describes the set of possible iteration vectors.
  ///
  /// In this case it is:
  ///
  ///     Domain: 0 <= i <= 100 + b
  ///             0 <= j <= i
  ///
  /// A pair of statement and iteration vector (S, (5,3)) is called statement
  /// instance.
  isl_set *Domain;

  /// The memory accesses of this statement.
  ///
  /// The only side effects of a statement are its memory accesses.
  typedef SmallVector<MemoryAccess *, 8> MemoryAccessVec;
  MemoryAccessVec MemAccs;

  /// @brief Mapping from instructions to (scalar) memory accesses.
  DenseMap<const Instruction *, MemoryAccessList *> InstructionToAccess;

  //@}

  /// @brief A SCoP statement represents either a basic block (affine/precise
  ///        case) or a whole region (non-affine case). Only one of the
  ///        following two members will therefore be set and indicate which
  ///        kind of statement this is.
  ///
  ///{

  /// @brief The BasicBlock represented by this statement (in the affine case).
  BasicBlock *BB;

  /// @brief The region represented by this statement (in the non-affine case).
  Region *R;

  ///}

  /// @brief The isl AST build for the new generated AST.
  isl_ast_build *Build;

  SmallVector<Loop *, 4> NestLoops;

  std::string BaseName;

  /// Build the statement.
  //@{
  void buildDomain();

  /// @brief Fill NestLoops with loops surrounding this statement.
  void collectSurroundingLoops();

  /// @brief Build the access relation of all memory accesses.
  void buildAccessRelations();

  /// @brief Detect and mark reductions in the ScopStmt
  void checkForReductions();

  /// @brief Collect loads which might form a reduction chain with @p StoreMA
  void
  collectCandiateReductionLoads(MemoryAccess *StoreMA,
                                llvm::SmallVectorImpl<MemoryAccess *> &Loads);
  //@}

  /// @brief Derive assumptions about parameter values from GetElementPtrInst
  ///
  /// In case a GEP instruction references into a fixed size array e.g., an
  /// access A[i][j] into an array A[100x100], LLVM-IR does not guarantee that
  /// the subscripts always compute values that are within array bounds. In this
  /// function we derive the set of parameter values for which all accesses are
  /// within bounds and add the assumption that the scop is only every executed
  /// with this set of parameter values.
  ///
  /// Example:
  ///
  ///   void foo(float A[][20], long n, long m {
  ///     for (long i = 0; i < n; i++)
  ///       for (long j = 0; j < m; j++)
  ///         A[i][j] = ...
  ///
  /// This loop yields out-of-bound accesses if m is at least 20 and at the same
  /// time at least one iteration of the outer loop is executed. Hence, we
  /// assume:
  ///
  ///   n <= 0 or m <= 20.
  ///
  /// TODO: The location where the GEP instruction is executed is not
  /// necessarily the location where the memory is actually accessed. As a
  /// result scanning for GEP[s] is imprecise. Even though this is not a
  /// correctness problem, this imprecision may result in missed optimizations
  /// or non-optimal run-time checks.
  void deriveAssumptionsFromGEP(GetElementPtrInst *Inst);

  /// @brief Scan @p Block and derive assumptions about parameter values.
  void deriveAssumptions(BasicBlock *Block);

public:
  ~ScopStmt();

  /// @brief Get an isl_ctx pointer.
  isl_ctx *getIslCtx() const;

  /// @brief Get the iteration domain of this ScopStmt.
  ///
  /// @return The iteration domain of this ScopStmt.
  __isl_give isl_set *getDomain() const;

  /// @brief Get the space of the iteration domain
  ///
  /// @return The space of the iteration domain
  __isl_give isl_space *getDomainSpace() const;

  /// @brief Get the id of the iteration domain space
  ///
  /// @return The id of the iteration domain space
  __isl_give isl_id *getDomainId() const;

  /// @brief Get an isl string representing this domain.
  std::string getDomainStr() const;

  /// @brief Get the schedule function of this ScopStmt.
  ///
  /// @return The schedule function of this ScopStmt.
  __isl_give isl_map *getSchedule() const;

  /// @brief Get an isl string representing this schedule.
  std::string getScheduleStr() const;

  /// @brief Get the BasicBlock represented by this ScopStmt (if any).
  ///
  /// @return The BasicBlock represented by this ScopStmt, or null if the
  ///         statement represents a region.
  BasicBlock *getBasicBlock() const { return BB; }

  /// @brief Return true if this statement represents a single basic block.
  bool isBlockStmt() const { return BB != nullptr; }

  /// @brief Get the region represented by this ScopStmt (if any).
  ///
  /// @return The region represented by this ScopStmt, or null if the statement
  ///         represents a basic block.
  Region *getRegion() const { return R; }

  /// @brief Return true if this statement represents a whole region.
  bool isRegionStmt() const { return R != nullptr; }

  /// @brief Return true if this statement does not contain any accesses.
  bool isEmpty() const { return MemAccs.empty(); }

  /// @brief Return the (scalar) memory accesses for @p Inst.
  const MemoryAccessList &getAccessesFor(const Instruction *Inst) const {
    MemoryAccessList *MAL = lookupAccessesFor(Inst);
    assert(MAL && "Cannot get memory accesses because they do not exist!");
    return *MAL;
  }

  /// @brief Return the (scalar) memory accesses for @p Inst if any.
  MemoryAccessList *lookupAccessesFor(const Instruction *Inst) const {
    auto It = InstructionToAccess.find(Inst);
    if (It == InstructionToAccess.end())
      return nullptr;
    return It->getSecond()->empty() ? nullptr : It->getSecond();
  }

  /// @brief Return the __first__ (scalar) memory access for @p Inst.
  const MemoryAccess &getAccessFor(const Instruction *Inst) const {
    MemoryAccess *MA = lookupAccessFor(Inst);
    assert(MA && "Cannot get memory access because it does not exist!");
    return *MA;
  }

  /// @brief Return the only array access for @p Inst.
  ///
  /// @param Inst The instruction for which to look up the access.
  /// @returns The unique array memory access related to Inst.
  MemoryAccess &getArrayAccessFor(const Instruction *Inst) const {
    auto It = InstructionToAccess.find(Inst);
    assert(It != InstructionToAccess.end() &&
           "No memory accesses found for instruction");
    auto *Accesses = It->getSecond();

    assert(Accesses && "No memory accesses found for instruction");

    MemoryAccess *ArrayAccess = nullptr;

    for (auto Access : *Accesses) {
      if (!Access->isArrayKind())
        continue;

      assert(!ArrayAccess && "More then one array access for instruction");

      ArrayAccess = Access;
    }

    assert(ArrayAccess && "No array access found for instruction!");
    return *ArrayAccess;
  }

  /// @brief Return the __first__ (scalar) memory access for @p Inst if any.
  MemoryAccess *lookupAccessFor(const Instruction *Inst) const {
    auto It = InstructionToAccess.find(Inst);
    if (It == InstructionToAccess.end())
      return nullptr;
    return It->getSecond()->empty() ? nullptr : It->getSecond()->front();
  }

  void setBasicBlock(BasicBlock *Block) {
    // TODO: Handle the case where the statement is a region statement, thus
    //       the entry block was split and needs to be changed in the region R.
    assert(BB && "Cannot set a block for a region statement");
    BB = Block;
  }

  /// @brief Add @p Access to this statement's list of accesses.
  void addAccess(MemoryAccess *Access);

  /// @brief Remove the memory access in @p InvMAs.
  ///
  /// Note that scalar accesses that are caused by any access in @p InvMAs will
  /// be eliminated too.
  void removeMemoryAccesses(MemoryAccessList &InvMAs);

  typedef MemoryAccessVec::iterator iterator;
  typedef MemoryAccessVec::const_iterator const_iterator;

  iterator begin() { return MemAccs.begin(); }
  iterator end() { return MemAccs.end(); }
  const_iterator begin() const { return MemAccs.begin(); }
  const_iterator end() const { return MemAccs.end(); }

  unsigned getNumParams() const;
  unsigned getNumIterators() const;

  Scop *getParent() { return &Parent; }
  const Scop *getParent() const { return &Parent; }

  const char *getBaseName() const;

  /// @brief Set the isl AST build.
  void setAstBuild(__isl_keep isl_ast_build *B) { Build = B; }

  /// @brief Get the isl AST build.
  __isl_keep isl_ast_build *getAstBuild() const { return Build; }

  /// @brief Restrict the domain of the statement.
  ///
  /// @param NewDomain The new statement domain.
  void restrictDomain(__isl_take isl_set *NewDomain);

  /// @brief Compute the isl representation for the SCEV @p E in this stmt.
  __isl_give isl_pw_aff *getPwAff(const SCEV *E);

  /// @brief Get the loop for a dimension.
  ///
  /// @param Dimension The dimension of the induction variable
  /// @return The loop at a certain dimension.
  const Loop *getLoopForDimension(unsigned Dimension) const;

  /// @brief Align the parameters in the statement to the scop context
  void realignParams();

  /// @brief Print the ScopStmt.
  ///
  /// @param OS The output stream the ScopStmt is printed to.
  void print(raw_ostream &OS) const;

  /// @brief Print the ScopStmt to stderr.
  void dump() const;
};

/// @brief Print ScopStmt S to raw_ostream O.
static inline raw_ostream &operator<<(raw_ostream &O, const ScopStmt &S) {
  S.print(O);
  return O;
}

///===----------------------------------------------------------------------===//
/// @brief Static Control Part
///
/// A Scop is the polyhedral representation of a control flow region detected
/// by the Scop detection. It is generated by translating the LLVM-IR and
/// abstracting its effects.
///
/// A Scop consists of a set of:
///
///   * A set of statements executed in the Scop.
///
///   * A set of global parameters
///   Those parameters are scalar integer values, which are constant during
///   execution.
///
///   * A context
///   This context contains information about the values the parameters
///   can take and relations between different parameters.
class Scop {
public:
  /// @brief Type to represent a pair of minimal/maximal access to an array.
  using MinMaxAccessTy = std::pair<isl_pw_multi_aff *, isl_pw_multi_aff *>;

  /// @brief Vector of minimal/maximal accesses to different arrays.
  using MinMaxVectorTy = SmallVector<MinMaxAccessTy, 4>;

  /// @brief Pair of minimal/maximal access vectors representing
  /// read write and read only accesses
  using MinMaxVectorPairTy = std::pair<MinMaxVectorTy, MinMaxVectorTy>;

  /// @brief Vector of pair of minimal/maximal access vectors representing
  /// non read only and read only accesses for each alias group.
  using MinMaxVectorPairVectorTy = SmallVector<MinMaxVectorPairTy, 4>;

private:
  Scop(const Scop &) = delete;
  const Scop &operator=(const Scop &) = delete;

  LoopInfo &LI;
  DominatorTree &DT;
  ScalarEvolution *SE;

  /// @brief The scop detection analysis.
  ScopDetection &SD;

  /// The underlying Region.
  Region &R;

  // Access function of bbs.
  AccFuncMapType &AccFuncMap;

  /// Flag to indicate that the scheduler actually optimized the SCoP.
  bool IsOptimized;

  /// @brief True if the underlying region has a single exiting block.
  bool HasSingleExitEdge;

  /// @brief Flag to remember if the SCoP contained an error block or not.
  bool HasErrorBlock;

  /// Max loop depth.
  unsigned MaxLoopDepth;

  typedef std::list<ScopStmt> StmtSet;
  /// The statements in this Scop.
  StmtSet Stmts;

  /// Parameters of this Scop
  typedef SmallVector<const SCEV *, 8> ParamVecType;
  ParamVecType Parameters;

  /// The isl_ids that are used to represent the parameters
  typedef std::map<const SCEV *, int> ParamIdType;
  ParamIdType ParameterIds;

  /// Isl context.
  isl_ctx *IslCtx;

  /// @brief A map from basic blocks to SCoP statements.
  DenseMap<BasicBlock *, ScopStmt *> StmtMap;

  /// @brief A map from basic blocks to their domains.
  DenseMap<BasicBlock *, isl_set *> DomainMap;

  /// Constraints on parameters.
  isl_set *Context;

  /// @brief The affinator used to translate SCEVs to isl expressions.
  SCEVAffinator Affinator;

  typedef MapVector<std::pair<AssertingVH<const Value>, int>,
                    std::unique_ptr<ScopArrayInfo>>
      ArrayInfoMapTy;
  /// @brief A map to remember ScopArrayInfo objects for all base pointers.
  ///
  /// As PHI nodes may have two array info objects associated, we add a flag
  /// that distinguishes between the PHI node specific ArrayInfo object
  /// and the normal one.
  ArrayInfoMapTy ScopArrayInfoMap;

  /// @brief The assumptions under which this scop was built.
  ///
  /// When constructing a scop sometimes the exact representation of a statement
  /// or condition would be very complex, but there is a common case which is a
  /// lot simpler, but which is only valid under certain assumptions. The
  /// assumed context records the assumptions taken during the construction of
  /// this scop and that need to be code generated as a run-time test.
  isl_set *AssumedContext;

  /// @brief The boundary assumptions under which this scop was built.
  ///
  /// The boundary context is similar to the assumed context as it contains
  /// constraints over the parameters we assume to be true. However, the
  /// boundary context is less useful for dependence analysis and
  /// simplification purposes as it contains only constraints that affect the
  /// boundaries of the parameter ranges. As these constraints can become quite
  /// complex, the boundary context and the assumed context are separated as a
  /// meassure to save compile time.
  isl_set *BoundaryContext;

  /// @brief The schedule of the SCoP
  ///
  /// The schedule of the SCoP describes the execution order of the statements
  /// in the scop by assigning each statement instance a possibly
  /// multi-dimensional execution time. The schedule is stored as a tree of
  /// schedule nodes.
  ///
  /// The most common nodes in a schedule tree are so-called band nodes. Band
  /// nodes map statement instances into a multi dimensional schedule space.
  /// This space can be seen as a multi-dimensional clock.
  ///
  /// Example:
  ///
  /// <S,(5,4)>  may be mapped to (5,4) by this schedule:
  ///
  /// s0 = i (Year of execution)
  /// s1 = j (Day of execution)
  ///
  /// or to (9, 20) by this schedule:
  ///
  /// s0 = i + j (Year of execution)
  /// s1 = 20 (Day of execution)
  ///
  /// The order statement instances are executed is defined by the
  /// schedule vectors they are mapped to. A statement instance
  /// <A, (i, j, ..)> is executed before a statement instance <B, (i', ..)>, if
  /// the schedule vector of A is lexicographic smaller than the schedule
  /// vector of B.
  ///
  /// Besides band nodes, schedule trees contain additional nodes that specify
  /// a textual ordering between two subtrees or filter nodes that filter the
  /// set of statement instances that will be scheduled in a subtree. There
  /// are also several other nodes. A full description of the different nodes
  /// in a schedule tree is given in the isl manual.
  isl_schedule *Schedule;

  /// @brief The set of minimal/maximal accesses for each alias group.
  ///
  /// When building runtime alias checks we look at all memory instructions and
  /// build so called alias groups. Each group contains a set of accesses to
  /// different base arrays which might alias with each other. However, between
  /// alias groups there is no aliasing possible.
  ///
  /// In a program with int and float pointers annotated with tbaa information
  /// we would probably generate two alias groups, one for the int pointers and
  /// one for the float pointers.
  ///
  /// During code generation we will create a runtime alias check for each alias
  /// group to ensure the SCoP is executed in an alias free environment.
  MinMaxVectorPairVectorTy MinMaxAliasGroups;

  /// @brief Mapping from invariant loads to the representing invariant load of
  ///        their equivalence class.
  ValueToValueMap InvEquivClassVMap;

  /// @brief List of invariant accesses.
  InvariantEquivClassesTy InvariantEquivClasses;

  /// @brief Scop constructor; invoked from ScopInfo::buildScop.
  Scop(Region &R, AccFuncMapType &AccFuncMap, ScopDetection &SD,
       ScalarEvolution &SE, DominatorTree &DT, LoopInfo &LI, isl_ctx *ctx,
       unsigned MaxLoopDepth);

  /// @brief Initialize this ScopInfo .
  void init(AliasAnalysis &AA, AssumptionCache &AC);

  /// @brief Add loop carried constraints to the header block of the loop @p L.
  ///
  /// @param L  The loop to process.
  void addLoopBoundsToHeaderDomain(Loop *L);

  /// @brief Compute the branching constraints for each basic block in @p R.
  ///
  /// @param R  The region we currently build branching conditions for.
  void buildDomainsWithBranchConstraints(Region *R);

  /// @brief Propagate the domain constraints through the region @p R.
  ///
  /// @param R  The region we currently build branching conditions for.
  void propagateDomainConstraints(Region *R);

  /// @brief Remove domains of error blocks/regions (and blocks dominated by
  ///        them).
  void removeErrorBlockDomains();

  /// @brief Compute the domain for each basic block in @p R.
  ///
  /// @param R  The region we currently traverse.
  void buildDomains(Region *R);

  /// @brief Check if a region part should be represented in the SCoP or not.
  ///
  /// If @p RN does not contain any useful calculation or is only reachable
  /// via error blocks we do not model it in the polyhedral representation.
  ///
  /// @param RN The region part to check.
  ///
  /// @return True if the part should be ignored, otherwise false.
  bool isIgnored(RegionNode *RN);

  /// @brief Add parameter constraints to @p C that imply a non-empty domain.
  __isl_give isl_set *addNonEmptyDomainConstraints(__isl_take isl_set *C) const;

  /// @brief Simplify the SCoP representation
  ///
  /// At the moment we perform the following simplifications:
  ///   - removal of no-op statements
  /// @param RemoveIgnoredStmts If true, also removed ignored statments.
  /// @see isIgnored()
  void simplifySCoP(bool RemoveIgnoredStmts);

  /// @brief Create equivalence classes for required invariant accesses.
  ///
  /// These classes will consolidate multiple required invariant loads from the
  /// same address in order to keep the number of dimensions in the SCoP
  /// description small. For each such class equivalence class only one
  /// representing element, hence one required invariant load, will be chosen
  /// and modeled as parameter. The method
  /// Scop::getRepresentingInvariantLoadSCEV() will replace each element from an
  /// equivalence class with the representing element that is modeled. As a
  /// consequence Scop::getIdForParam() will only return an id for the
  /// representing element of each equivalence class, thus for each required
  /// invariant location.
  void buildInvariantEquivalenceClasses();

  /// @brief Check if a memory access can be hoisted.
  ///
  /// @param Access The access to verify.
  /// @param Writes The set of all memory writes in the scop.
  ///
  /// @return Return true if a memory access can be hoisted.
  bool isHoistableAccess(MemoryAccess *Access,
                         __isl_keep isl_union_map *Writes);

  /// @brief Verify that all required invariant loads have been hoisted.
  ///
  /// Invariant load hoisting is not guaranteed to hoist all loads that were
  /// assumed to be scop invariant during scop detection. This function checks
  /// for cases where the hoisting failed, but where it would have been
  /// necessary for our scop modeling to be correct. In case of insufficent
  /// hoisting the scop is marked as invalid.
  ///
  /// In the example below Bound[1] is required to be invariant:
  ///
  /// for (int i = 1; i < Bound[0]; i++)
  ///   for (int j = 1; j < Bound[1]; j++)
  ///     ...
  ///
  void verifyInvariantLoads();

  /// @brief Hoist invariant memory loads and check for required ones.
  ///
  /// We first identify "common" invariant loads, thus loads that are invariant
  /// and can be hoisted. Then we check if all required invariant loads have
  /// been identified as (common) invariant. A load is a required invariant load
  /// if it was assumed to be invariant during SCoP detection, e.g., to assume
  /// loop bounds to be affine or runtime alias checks to be placeable. In case
  /// a required invariant load was not identified as (common) invariant we will
  /// drop this SCoP. An example for both "common" as well as required invariant
  /// loads is given below:
  ///
  /// for (int i = 1; i < *LB[0]; i++)
  ///   for (int j = 1; j < *LB[1]; j++)
  ///     A[i][j] += A[0][0] + (*V);
  ///
  /// Common inv. loads: V, A[0][0], LB[0], LB[1]
  /// Required inv. loads: LB[0], LB[1], (V, if it may alias with A or LB)
  void hoistInvariantLoads();

  /// @brief Add invariant loads listed in @p InvMAs with the domain of @p Stmt.
  void addInvariantLoads(ScopStmt &Stmt, MemoryAccessList &InvMAs);

  /// @brief Build the Context of the Scop.
  void buildContext();

  /// @brief Build the BoundaryContext based on the wrapping of expressions.
  void buildBoundaryContext();

  /// @brief Add user provided parameter constraints to context (source code).
  void addUserAssumptions(AssumptionCache &AC);

  /// @brief Add user provided parameter constraints to context (command line).
  void addUserContext();

  /// @brief Add the bounds of the parameters to the context.
  void addParameterBounds();

  /// @brief Simplify the assumed and boundary context.
  void simplifyContexts();

  /// @brief Get the representing SCEV for @p S if applicable, otherwise @p S.
  ///
  /// Invariant loads of the same location are put in an equivalence class and
  /// only one of them is chosen as a representing element that will be
  /// modeled as a parameter. The others have to be normalized, i.e.,
  /// replaced by the representing element of their equivalence class, in order
  /// to get the correct parameter value, e.g., in the SCEVAffinator.
  ///
  /// @param S The SCEV to normalize.
  ///
  /// @return The representing SCEV for invariant loads or @p S if none.
  const SCEV *getRepresentingInvariantLoadSCEV(const SCEV *S);

  /// @brief Create a new SCoP statement for either @p BB or @p R.
  ///
  /// Either @p BB or @p R should be non-null. A new statement for the non-null
  /// argument will be created and added to the statement vector and map.
  ///
  /// @param BB         The basic block we build the statement for (or null)
  /// @param R          The region we build the statement for (or null).
  void addScopStmt(BasicBlock *BB, Region *R);

  /// @param Update access dimensionalities.
  ///
  /// When detecting memory accesses different accesses to the same array may
  /// have built with different dimensionality, as outer zero-values dimensions
  /// may not have been recognized as separate dimensions. This function goes
  /// again over all memory accesses and updates their dimensionality to match
  /// the dimensionality of the underlying ScopArrayInfo object.
  void updateAccessDimensionality();

  /// @brief Build Schedule and ScopStmts.
  ///
  /// @param R              The current region traversed.
  /// @param LoopSchedules  Map from loops to their schedule and progress.
  void buildSchedule(
      Region *R,
      DenseMap<Loop *, std::pair<isl_schedule *, unsigned>> &LoopSchedules);

  /// @brief Collect all memory access relations of a given type.
  ///
  /// @param Predicate A predicate function that returns true if an access is
  ///                  of a given type.
  ///
  /// @returns The set of memory accesses in the scop that match the predicate.
  __isl_give isl_union_map *
  getAccessesOfType(std::function<bool(MemoryAccess &)> Predicate);

  /// @name Helper function for printing the Scop.
  ///
  ///{
  void printContext(raw_ostream &OS) const;
  void printArrayInfo(raw_ostream &OS) const;
  void printStatements(raw_ostream &OS) const;
  void printAliasAssumptions(raw_ostream &OS) const;
  ///}

  friend class ScopInfo;

public:
  ~Scop();

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

  ScalarEvolution *getSE() const;
  ScopDetection &getSD() const { return SD; }

  /// @brief Get the count of parameters used in this Scop.
  ///
  /// @return The count of parameters used in this Scop.
  inline ParamVecType::size_type getNumParams() const {
    return Parameters.size();
  }

  /// @brief Get a set containing the parameters used in this Scop
  ///
  /// @return The set containing the parameters used in this Scop.
  inline const ParamVecType &getParams() const { return Parameters; }

  /// @brief Take a list of parameters and add the new ones to the scop.
  void addParams(std::vector<const SCEV *> NewParameters);

  int getNumArrays() { return ScopArrayInfoMap.size(); }

  /// @brief Return whether this scop is empty, i.e. contains no statements that
  /// could be executed.
  bool isEmpty() const { return Stmts.empty(); }

  typedef iterator_range<ArrayInfoMapTy::iterator> array_range;
  typedef iterator_range<ArrayInfoMapTy::const_iterator> const_array_range;

  inline array_range arrays() {
    return array_range(ScopArrayInfoMap.begin(), ScopArrayInfoMap.end());
  }

  inline const_array_range arrays() const {
    return const_array_range(ScopArrayInfoMap.begin(), ScopArrayInfoMap.end());
  }

  /// @brief Return the isl_id that represents a certain parameter.
  ///
  /// @param Parameter A SCEV that was recognized as a Parameter.
  ///
  /// @return The corresponding isl_id or NULL otherwise.
  isl_id *getIdForParam(const SCEV *Parameter);

  /// @name Parameter Iterators
  ///
  /// These iterators iterate over all parameters of this Scop.
  //@{
  typedef ParamVecType::iterator param_iterator;
  typedef ParamVecType::const_iterator const_param_iterator;

  param_iterator param_begin() { return Parameters.begin(); }
  param_iterator param_end() { return Parameters.end(); }
  const_param_iterator param_begin() const { return Parameters.begin(); }
  const_param_iterator param_end() const { return Parameters.end(); }
  //@}

  /// @brief Get the maximum region of this static control part.
  ///
  /// @return The maximum region of this static control part.
  inline const Region &getRegion() const { return R; }
  inline Region &getRegion() { return R; }

  /// @brief Get the maximum depth of the loop.
  ///
  /// @return The maximum depth of the loop.
  inline unsigned getMaxLoopDepth() const { return MaxLoopDepth; }

  /// @brief Return the invariant equivalence class for @p Val if any.
  const InvariantEquivClassTy *lookupInvariantEquivClass(Value *Val) const;

  /// @brief Return the set of invariant accesses.
  const InvariantEquivClassesTy &getInvariantAccesses() const {
    return InvariantEquivClasses;
  }

  /// @brief Mark the SCoP as optimized by the scheduler.
  void markAsOptimized() { IsOptimized = true; }

  /// @brief Check if the SCoP has been optimized by the scheduler.
  bool isOptimized() const { return IsOptimized; }

  /// @brief Get the name of this Scop.
  std::string getNameStr() const;

  /// @brief Get the constraint on parameter of this Scop.
  ///
  /// @return The constraint on parameter of this Scop.
  __isl_give isl_set *getContext() const;
  __isl_give isl_space *getParamSpace() const;

  /// @brief Get the assumed context for this Scop.
  ///
  /// @return The assumed context of this Scop.
  __isl_give isl_set *getAssumedContext() const;

  /// @brief Get the runtime check context for this Scop.
  ///
  /// The runtime check context contains all constraints that have to
  /// hold at runtime for the optimized version to be executed.
  ///
  /// @return The runtime check context of this Scop.
  __isl_give isl_set *getRuntimeCheckContext() const;

  /// @brief Return true if the optimized SCoP can be executed.
  ///
  /// In addition to the runtime check context this will also utilize the domain
  /// constraints to decide it the optimized version can actually be executed.
  ///
  /// @returns True if the optimized SCoP can be executed.
  bool hasFeasibleRuntimeContext() const;

  /// @brief Track and report an assumption.
  ///
  /// Use 'clang -Rpass-analysis=polly-scops' or 'opt -pass-remarks=polly-scops'
  /// to output the assumptions.
  ///
  /// @param Kind The assumption kind describing the underlying cause.
  /// @param Set  The relations between parameters that are assumed to hold.
  /// @param Loc  The location in the source that caused this assumption.
  void trackAssumption(AssumptionKind Kind, __isl_keep isl_set *Set,
                       DebugLoc Loc);

  /// @brief Add assumptions to assumed context.
  ///
  /// The assumptions added will be assumed to hold during the execution of the
  /// scop. However, as they are generally not statically provable, at code
  /// generation time run-time checks will be generated that ensure the
  /// assumptions hold.
  ///
  /// WARNING: We currently exploit in simplifyAssumedContext the knowledge
  ///          that assumptions do not change the set of statement instances
  ///          executed.
  ///
  /// @param Kind The assumption kind describing the underlying cause.
  /// @param Set  The relations between parameters that are assumed to hold.
  /// @param Loc  The location in the source that caused this assumption.
  void addAssumption(AssumptionKind Kind, __isl_take isl_set *Set,
                     DebugLoc Loc);

  /// @brief Mark the scop as invalid.
  ///
  /// This method adds an assumption to the scop that is always invalid. As a
  /// result, the scop will not be optimized later on. This function is commonly
  /// called when a condition makes it impossible (or too compile time
  /// expensive) to process this scop any further.
  ///
  /// @param Kind The assumption kind describing the underlying cause.
  /// @param Loc  The location in the source that triggered .
  void invalidate(AssumptionKind Kind, DebugLoc Loc);

  /// @brief Get the boundary context for this Scop.
  ///
  /// @return The boundary context of this Scop.
  __isl_give isl_set *getBoundaryContext() const;

  /// @brief Build the alias checks for this SCoP.
  void buildAliasChecks(AliasAnalysis &AA);

  /// @brief Build all alias groups for this SCoP.
  ///
  /// @returns True if __no__ error occurred, false otherwise.
  bool buildAliasGroups(AliasAnalysis &AA);

  /// @brief Return all alias groups for this SCoP.
  const MinMaxVectorPairVectorTy &getAliasGroups() const {
    return MinMaxAliasGroups;
  }

  /// @brief Get an isl string representing the context.
  std::string getContextStr() const;

  /// @brief Get an isl string representing the assumed context.
  std::string getAssumedContextStr() const;

  /// @brief Get an isl string representing the boundary context.
  std::string getBoundaryContextStr() const;

  /// @brief Return the stmt for the given @p BB or nullptr if none.
  ScopStmt *getStmtForBasicBlock(BasicBlock *BB) const;

  /// @brief Return the ScopStmt that represents @p RN; can return nullptr if
  /// the RegionNode is not within the SCoP or has been removed due to
  /// simplifications.
  ScopStmt *getStmtForRegionNode(RegionNode *RN) const;

  /// @brief Return the number of statements in the SCoP.
  size_t getSize() const { return Stmts.size(); }

  /// @name Statements Iterators
  ///
  /// These iterators iterate over all statements of this Scop.
  //@{
  typedef StmtSet::iterator iterator;
  typedef StmtSet::const_iterator const_iterator;

  iterator begin() { return Stmts.begin(); }
  iterator end() { return Stmts.end(); }
  const_iterator begin() const { return Stmts.begin(); }
  const_iterator end() const { return Stmts.end(); }

  typedef StmtSet::reverse_iterator reverse_iterator;
  typedef StmtSet::const_reverse_iterator const_reverse_iterator;

  reverse_iterator rbegin() { return Stmts.rbegin(); }
  reverse_iterator rend() { return Stmts.rend(); }
  const_reverse_iterator rbegin() const { return Stmts.rbegin(); }
  const_reverse_iterator rend() const { return Stmts.rend(); }
  //@}

  /// @brief Return the (possibly new) ScopArrayInfo object for @p Access.
  ///
  /// @param ElementType The type of the elements stored in this array.
  /// @param Kind        The kind of the array info object.
  const ScopArrayInfo *getOrCreateScopArrayInfo(Value *BasePtr,
                                                Type *ElementType,
                                                ArrayRef<const SCEV *> Sizes,
                                                ScopArrayInfo::MemoryKind Kind);

  /// @brief Return the cached ScopArrayInfo object for @p BasePtr.
  ///
  /// @param BasePtr   The base pointer the object has been stored for.
  /// @param Kind      The kind of array info object.
  const ScopArrayInfo *getScopArrayInfo(Value *BasePtr,
                                        ScopArrayInfo::MemoryKind Kind);

  void setContext(isl_set *NewContext);

  /// @brief Align the parameters in the statement to the scop context
  void realignParams();

  /// @brief Return true if the SCoP contained at least one error block.
  bool hasErrorBlock() const { return HasErrorBlock; }

  /// @brief Return true if the underlying region has a single exiting block.
  bool hasSingleExitEdge() const { return HasSingleExitEdge; }

  /// @brief Print the static control part.
  ///
  /// @param OS The output stream the static control part is printed to.
  void print(raw_ostream &OS) const;

  /// @brief Print the ScopStmt to stderr.
  void dump() const;

  /// @brief Get the isl context of this static control part.
  ///
  /// @return The isl context of this static control part.
  isl_ctx *getIslCtx() const;

  /// @brief Compute the isl representation for the SCEV @p
  ///
  /// @param BB An (optional) basic block in which the isl_pw_aff is computed.
  ///           SCEVs known to not reference any loops in the SCoP can be
  ///           passed without a @p BB.
  __isl_give isl_pw_aff *getPwAff(const SCEV *E, BasicBlock *BB = nullptr);

  /// @brief Return the non-loop carried conditions on the domain of @p Stmt.
  ///
  /// @param Stmt The statement for which the conditions should be returned.
  __isl_give isl_set *getDomainConditions(ScopStmt *Stmt);

  /// @brief Return the non-loop carried conditions on the domain of @p BB.
  ///
  /// @param BB The block for which the conditions should be returned.
  __isl_give isl_set *getDomainConditions(BasicBlock *BB);

  /// @brief Get a union set containing the iteration domains of all statements.
  __isl_give isl_union_set *getDomains() const;

  /// @brief Get a union map of all may-writes performed in the SCoP.
  __isl_give isl_union_map *getMayWrites();

  /// @brief Get a union map of all must-writes performed in the SCoP.
  __isl_give isl_union_map *getMustWrites();

  /// @brief Get a union map of all writes performed in the SCoP.
  __isl_give isl_union_map *getWrites();

  /// @brief Get a union map of all reads performed in the SCoP.
  __isl_give isl_union_map *getReads();

  /// @brief Get a union map of all memory accesses performed in the SCoP.
  __isl_give isl_union_map *getAccesses();

  /// @brief Get the schedule of all the statements in the SCoP.
  __isl_give isl_union_map *getSchedule() const;

  /// @brief Get a schedule tree describing the schedule of all statements.
  __isl_give isl_schedule *getScheduleTree() const;

  /// @brief Update the current schedule
  ///
  /// @brief NewSchedule The new schedule (given as a flat union-map).
  void setSchedule(__isl_take isl_union_map *NewSchedule);

  /// @brief Update the current schedule
  ///
  /// @brief NewSchedule The new schedule (given as schedule tree).
  void setScheduleTree(__isl_take isl_schedule *NewSchedule);

  /// @brief Intersects the domains of all statements in the SCoP.
  ///
  /// @return true if a change was made
  bool restrictDomains(__isl_take isl_union_set *Domain);

  /// @brief Get the depth of a loop relative to the outermost loop in the Scop.
  ///
  /// This will return
  ///    0 if @p L is an outermost loop in the SCoP
  ///   >0 for other loops in the SCoP
  ///   -1 if @p L is nullptr or there is no outermost loop in the SCoP
  int getRelativeLoopDepth(const Loop *L) const;
};

/// @brief Print Scop scop to raw_ostream O.
static inline raw_ostream &operator<<(raw_ostream &O, const Scop &scop) {
  scop.print(O);
  return O;
}

///===---------------------------------------------------------------------===//
/// @brief Build the Polly IR (Scop and ScopStmt) on a Region.
///
class ScopInfo : public RegionPass {
  //===-------------------------------------------------------------------===//
  ScopInfo(const ScopInfo &) = delete;
  const ScopInfo &operator=(const ScopInfo &) = delete;

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

  // DominatorTree to reason about guaranteed execution.
  DominatorTree *DT;

  // Access function of statements (currently BasicBlocks) .
  //
  // This owns all the MemoryAccess objects of the Scop created in this pass. It
  // must live until #scop is deleted.
  AccFuncMapType AccFuncMap;

  // The Scop
  Scop *scop;
  isl_ctx *ctx;

  /// @brief Return the SCoP region that is currently processed.
  Region *getRegion() const {
    if (!scop)
      return nullptr;
    return &scop->getRegion();
  }

  // Clear the context.
  void clear();

  // Build the SCoP for Region @p R.
  void buildScop(Region &R, AssumptionCache &AC);

  /// @brief Build an instance of MemoryAccess from the Load/Store instruction.
  ///
  /// @param Inst       The Load/Store instruction that access the memory
  /// @param L          The parent loop of the instruction
  /// @param R          The region on which to build the data access dictionary.
  /// @param BoxedLoops The set of loops that are overapproximated in @p R.
  /// @param ScopRIL    The required invariant loads equivalence classes.
  void buildMemoryAccess(Instruction *Inst, Loop *L, Region *R,
                         const ScopDetection::BoxedLoopsSetTy *BoxedLoops,
                         const InvariantLoadsSetTy &ScopRIL);

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

  /// @brief Create MemoryAccesses for the given PHI node in the given region.
  ///
  /// @param PHI                The PHI node to be handled
  /// @param R                  The SCoP region
  /// @param NonAffineSubRegion The non affine sub-region @p PHI is in.
  /// @param IsExitBlock        Flag to indicate that @p PHI is in the exit BB.
  void buildPHIAccesses(PHINode *PHI, Region &R, Region *NonAffineSubRegion,
                        bool IsExitBlock = false);

  /// @brief Build the access functions for the subregion @p SR.
  ///
  /// @param R  The SCoP region.
  /// @param SR A subregion of @p R.
  void buildAccessFunctions(Region &R, Region &SR);

  /// @brief Create ScopStmt for all BBs and non-affine subregions of @p SR.
  ///
  /// Some of the statments might be optimized away later when they do not
  /// access any memory and thus have no effect.
  void buildStmts(Region &SR);

  /// @brief Build the access functions for the basic block @p BB
  ///
  /// @param R                  The SCoP region.
  /// @param BB                 A basic block in @p R.
  /// @param NonAffineSubRegion The non affine sub-region @p BB is in.
  /// @param IsExitBlock        Flag to indicate that @p BB is in the exit BB.
  void buildAccessFunctions(Region &R, BasicBlock &BB,
                            Region *NonAffineSubRegion = nullptr,
                            bool IsExitBlock = false);

  /// @brief Create a new MemoryAccess object and add it to #AccFuncMap.
  ///
  /// @param BB          The block where the access takes place.
  /// @param Inst        The instruction doing the access. It is not necessarily
  ///                    inside @p BB.
  /// @param Type        The kind of access.
  /// @param BaseAddress The accessed array's base address.
  /// @param ElemBytes   Size of accessed array element.
  /// @param Affine      Whether all subscripts are affine expressions.
  /// @param AccessValue Value read or written.
  /// @param Subscripts  Access subscripts per dimension.
  /// @param Sizes       The array diminsion's sizes.
  /// @param Kind        The kind of memory accessed.
  void addMemoryAccess(BasicBlock *BB, Instruction *Inst,
                       MemoryAccess::AccessType Type, Value *BaseAddress,
                       unsigned ElemBytes, bool Affine, Value *AccessValue,
                       ArrayRef<const SCEV *> Subscripts,
                       ArrayRef<const SCEV *> Sizes,
                       ScopArrayInfo::MemoryKind Kind);

  /// @brief Create a MemoryAccess that represents either a LoadInst or
  /// StoreInst.
  ///
  /// @param MemAccInst  The LoadInst or StoreInst.
  /// @param Type        The kind of access.
  /// @param BaseAddress The accessed array's base address.
  /// @param ElemBytes   Size of accessed array element.
  /// @param IsAffine    Whether all subscripts are affine expressions.
  /// @param Subscripts  Access subscripts per dimension.
  /// @param Sizes       The array dimension's sizes.
  /// @param AccessValue Value read or written.
  /// @see ScopArrayInfo::MemoryKind
  void addArrayAccess(Instruction *MemAccInst, MemoryAccess::AccessType Type,
                      Value *BaseAddress, unsigned ElemBytes, bool IsAffine,
                      ArrayRef<const SCEV *> Subscripts,
                      ArrayRef<const SCEV *> Sizes, Value *AccessValue);

  /// @brief Create a MemoryAccess for writing an llvm::Value.
  ///
  /// The access will be created at the @p Value's definition.
  ///
  /// @param Value The value to be written.
  /// @see addValueReadAccess()
  /// @see ScopArrayInfo::MemoryKind
  void addValueWriteAccess(Instruction *Value);

  /// @brief Create a MemoryAccess for reloading an llvm::Value.
  ///
  /// Use this overload only for non-PHI instructions.
  ///
  /// @param Value The scalar expected to be loaded.
  /// @param User  User of the scalar; this is where the access is added.
  /// @see addValueWriteAccess()
  /// @see ScopArrayInfo::MemoryKind
  void addValueReadAccess(Value *Value, Instruction *User);

  /// @brief Create a MemoryAccess for reloading an llvm::Value.
  ///
  /// This is for PHINodes using the scalar. As we model it, the used value must
  /// be available at the incoming block instead of when hitting the
  /// instruction.
  ///
  /// @param Value  The scalar expected to be loaded.
  /// @param User   The PHI node referencing @p Value.
  /// @param UserBB Incoming block for the incoming @p Value.
  /// @see addPHIWriteAccess()
  /// @see addValueWriteAccess()
  /// @see ScopArrayInfo::MemoryKind
  void addValueReadAccess(Value *Value, PHINode *User, BasicBlock *UserBB);

  /// @brief Create a write MemoryAccess for the incoming block of a phi node.
  ///
  /// Each of the incoming blocks write their incoming value to be picked in the
  /// phi's block.
  ///
  /// @param PHI           PHINode under consideration.
  /// @param IncomingBlock Some predecessor block.
  /// @param IncomingValue @p PHI's value when coming from @p IncomingBlock.
  /// @param IsExitBlock   When true, uses the .s2a alloca instead of the
  ///                      .phiops one. Required for values escaping through a
  ///                      PHINode in the SCoP region's exit block.
  /// @see addPHIReadAccess()
  /// @see ScopArrayInfo::MemoryKind
  void addPHIWriteAccess(PHINode *PHI, BasicBlock *IncomingBlock,
                         Value *IncomingValue, bool IsExitBlock);

  /// @brief Create a MemoryAccess for reading the value of a phi.
  ///
  /// The modeling assumes that all incoming blocks write their incoming value
  /// to the same location. Thus, this access will read the incoming block's
  /// value as instructed by this @p PHI.
  ///
  /// @param PHI PHINode under consideration; the READ access will be added
  /// here.
  /// @see addPHIWriteAccess()
  /// @see ScopArrayInfo::MemoryKind
  void addPHIReadAccess(PHINode *PHI);

public:
  static char ID;
  explicit ScopInfo();
  ~ScopInfo();

  /// @brief Try to build the Polly IR of static control part on the current
  ///        SESE-Region.
  ///
  /// @return If the current region is a valid for a static control part,
  ///         return the Polly IR representing this static control part,
  ///         return null otherwise.
  Scop *getScop() { return scop; }
  const Scop *getScop() const { return scop; }

  /// @name RegionPass interface
  //@{
  virtual bool runOnRegion(Region *R, RGPassManager &RGM);
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  virtual void releaseMemory() { clear(); }
  virtual void print(raw_ostream &OS, const Module *) const;
  //@}
};

} // end namespace polly

namespace llvm {
class PassRegistry;
void initializeScopInfoPass(llvm::PassRegistry &);
}

#endif
