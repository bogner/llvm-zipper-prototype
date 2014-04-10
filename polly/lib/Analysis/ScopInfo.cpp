//===--------- ScopInfo.cpp  - Create Scops from LLVM IR ------------------===//
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
// This represantation is shared among several tools in the polyhedral
// community, which are e.g. Cloog, Pluto, Loopo, Graphite.
//
//===----------------------------------------------------------------------===//

#include "polly/CodeGen/BlockGenerators.h"
#include "polly/LinkAllPasses.h"
#include "polly/ScopInfo.h"
#include "polly/Support/GICHelper.h"
#include "polly/Support/SCEVValidator.h"
#include "polly/Support/ScopHelper.h"
#include "polly/TempScopInfo.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/RegionIterator.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "polly-scops"
#include "llvm/Support/Debug.h"

#include "isl/constraint.h"
#include "isl/set.h"
#include "isl/map.h"
#include "isl/union_map.h"
#include "isl/aff.h"
#include "isl/printer.h"
#include "isl/local_space.h"
#include "isl/options.h"
#include "isl/val.h"
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;
using namespace polly;

STATISTIC(ScopFound, "Number of valid Scops");
STATISTIC(RichScopFound, "Number of Scops containing a loop");

/// Translate a 'const SCEV *' expression in an isl_pw_aff.
struct SCEVAffinator : public SCEVVisitor<SCEVAffinator, isl_pw_aff *> {
public:
  /// @brief Translate a 'const SCEV *' to an isl_pw_aff.
  ///
  /// @param Stmt The location at which the scalar evolution expression
  ///             is evaluated.
  /// @param Expr The expression that is translated.
  static __isl_give isl_pw_aff *getPwAff(ScopStmt *Stmt, const SCEV *Expr);

private:
  isl_ctx *Ctx;
  int NbLoopSpaces;
  const Scop *S;

  SCEVAffinator(const ScopStmt *Stmt);
  int getLoopDepth(const Loop *L);

  __isl_give isl_pw_aff *visit(const SCEV *Expr);
  __isl_give isl_pw_aff *visitConstant(const SCEVConstant *Expr);
  __isl_give isl_pw_aff *visitTruncateExpr(const SCEVTruncateExpr *Expr);
  __isl_give isl_pw_aff *visitZeroExtendExpr(const SCEVZeroExtendExpr *Expr);
  __isl_give isl_pw_aff *visitSignExtendExpr(const SCEVSignExtendExpr *Expr);
  __isl_give isl_pw_aff *visitAddExpr(const SCEVAddExpr *Expr);
  __isl_give isl_pw_aff *visitMulExpr(const SCEVMulExpr *Expr);
  __isl_give isl_pw_aff *visitUDivExpr(const SCEVUDivExpr *Expr);
  __isl_give isl_pw_aff *visitAddRecExpr(const SCEVAddRecExpr *Expr);
  __isl_give isl_pw_aff *visitSMaxExpr(const SCEVSMaxExpr *Expr);
  __isl_give isl_pw_aff *visitUMaxExpr(const SCEVUMaxExpr *Expr);
  __isl_give isl_pw_aff *visitUnknown(const SCEVUnknown *Expr);

  friend struct SCEVVisitor<SCEVAffinator, isl_pw_aff *>;
};

SCEVAffinator::SCEVAffinator(const ScopStmt *Stmt)
    : Ctx(Stmt->getIslCtx()), NbLoopSpaces(Stmt->getNumIterators()),
      S(Stmt->getParent()) {}

__isl_give isl_pw_aff *SCEVAffinator::getPwAff(ScopStmt *Stmt,
                                               const SCEV *Scev) {
  Scop *S = Stmt->getParent();
  const Region *Reg = &S->getRegion();

  S->addParams(getParamsInAffineExpr(Reg, Scev, *S->getSE()));

  SCEVAffinator Affinator(Stmt);
  return Affinator.visit(Scev);
}

__isl_give isl_pw_aff *SCEVAffinator::visit(const SCEV *Expr) {
  // In case the scev is a valid parameter, we do not further analyze this
  // expression, but create a new parameter in the isl_pw_aff. This allows us
  // to treat subexpressions that we cannot translate into an piecewise affine
  // expression, as constant parameters of the piecewise affine expression.
  if (isl_id *Id = S->getIdForParam(Expr)) {
    isl_space *Space = isl_space_set_alloc(Ctx, 1, NbLoopSpaces);
    Space = isl_space_set_dim_id(Space, isl_dim_param, 0, Id);

    isl_set *Domain = isl_set_universe(isl_space_copy(Space));
    isl_aff *Affine = isl_aff_zero_on_domain(isl_local_space_from_space(Space));
    Affine = isl_aff_add_coefficient_si(Affine, isl_dim_param, 0, 1);

    return isl_pw_aff_alloc(Domain, Affine);
  }

  return SCEVVisitor<SCEVAffinator, isl_pw_aff *>::visit(Expr);
}

__isl_give isl_pw_aff *SCEVAffinator::visitConstant(const SCEVConstant *Expr) {
  ConstantInt *Value = Expr->getValue();
  isl_val *v;

  // LLVM does not define if an integer value is interpreted as a signed or
  // unsigned value. Hence, without further information, it is unknown how
  // this value needs to be converted to GMP. At the moment, we only support
  // signed operations. So we just interpret it as signed. Later, there are
  // two options:
  //
  // 1. We always interpret any value as signed and convert the values on
  //    demand.
  // 2. We pass down the signedness of the calculation and use it to interpret
  //    this constant correctly.
  v = isl_valFromAPInt(Ctx, Value->getValue(), /* isSigned */ true);

  isl_space *Space = isl_space_set_alloc(Ctx, 0, NbLoopSpaces);
  isl_local_space *ls = isl_local_space_from_space(isl_space_copy(Space));
  isl_aff *Affine = isl_aff_zero_on_domain(ls);
  isl_set *Domain = isl_set_universe(Space);

  Affine = isl_aff_add_constant_val(Affine, v);

  return isl_pw_aff_alloc(Domain, Affine);
}

__isl_give isl_pw_aff *
SCEVAffinator::visitTruncateExpr(const SCEVTruncateExpr *Expr) {
  llvm_unreachable("SCEVTruncateExpr not yet supported");
}

__isl_give isl_pw_aff *
SCEVAffinator::visitZeroExtendExpr(const SCEVZeroExtendExpr *Expr) {
  llvm_unreachable("SCEVZeroExtendExpr not yet supported");
}

__isl_give isl_pw_aff *
SCEVAffinator::visitSignExtendExpr(const SCEVSignExtendExpr *Expr) {
  // Assuming the value is signed, a sign extension is basically a noop.
  // TODO: Reconsider this as soon as we support unsigned values.
  return visit(Expr->getOperand());
}

__isl_give isl_pw_aff *SCEVAffinator::visitAddExpr(const SCEVAddExpr *Expr) {
  isl_pw_aff *Sum = visit(Expr->getOperand(0));

  for (int i = 1, e = Expr->getNumOperands(); i < e; ++i) {
    isl_pw_aff *NextSummand = visit(Expr->getOperand(i));
    Sum = isl_pw_aff_add(Sum, NextSummand);
  }

  // TODO: Check for NSW and NUW.

  return Sum;
}

__isl_give isl_pw_aff *SCEVAffinator::visitMulExpr(const SCEVMulExpr *Expr) {
  isl_pw_aff *Product = visit(Expr->getOperand(0));

  for (int i = 1, e = Expr->getNumOperands(); i < e; ++i) {
    isl_pw_aff *NextOperand = visit(Expr->getOperand(i));

    if (!isl_pw_aff_is_cst(Product) && !isl_pw_aff_is_cst(NextOperand)) {
      isl_pw_aff_free(Product);
      isl_pw_aff_free(NextOperand);
      return NULL;
    }

    Product = isl_pw_aff_mul(Product, NextOperand);
  }

  // TODO: Check for NSW and NUW.
  return Product;
}

__isl_give isl_pw_aff *SCEVAffinator::visitUDivExpr(const SCEVUDivExpr *Expr) {
  llvm_unreachable("SCEVUDivExpr not yet supported");
}

__isl_give isl_pw_aff *
SCEVAffinator::visitAddRecExpr(const SCEVAddRecExpr *Expr) {
  assert(Expr->isAffine() && "Only affine AddRecurrences allowed");

  // Directly generate isl_pw_aff for Expr if 'start' is zero.
  if (Expr->getStart()->isZero()) {
    assert(S->getRegion().contains(Expr->getLoop()) &&
           "Scop does not contain the loop referenced in this AddRec");

    isl_pw_aff *Start = visit(Expr->getStart());
    isl_pw_aff *Step = visit(Expr->getOperand(1));
    isl_space *Space = isl_space_set_alloc(Ctx, 0, NbLoopSpaces);
    isl_local_space *LocalSpace = isl_local_space_from_space(Space);

    int loopDimension = getLoopDepth(Expr->getLoop());

    isl_aff *LAff = isl_aff_set_coefficient_si(
        isl_aff_zero_on_domain(LocalSpace), isl_dim_in, loopDimension, 1);
    isl_pw_aff *LPwAff = isl_pw_aff_from_aff(LAff);

    // TODO: Do we need to check for NSW and NUW?
    return isl_pw_aff_add(Start, isl_pw_aff_mul(Step, LPwAff));
  }

  // Translate AddRecExpr from '{start, +, inc}' into 'start + {0, +, inc}'
  // if 'start' is not zero.
  ScalarEvolution &SE = *S->getSE();
  const SCEV *ZeroStartExpr = SE.getAddRecExpr(
      SE.getConstant(Expr->getStart()->getType(), 0),
      Expr->getStepRecurrence(SE), Expr->getLoop(), SCEV::FlagAnyWrap);

  isl_pw_aff *ZeroStartResult = visit(ZeroStartExpr);
  isl_pw_aff *Start = visit(Expr->getStart());

  return isl_pw_aff_add(ZeroStartResult, Start);
}

__isl_give isl_pw_aff *SCEVAffinator::visitSMaxExpr(const SCEVSMaxExpr *Expr) {
  isl_pw_aff *Max = visit(Expr->getOperand(0));

  for (int i = 1, e = Expr->getNumOperands(); i < e; ++i) {
    isl_pw_aff *NextOperand = visit(Expr->getOperand(i));
    Max = isl_pw_aff_max(Max, NextOperand);
  }

  return Max;
}

__isl_give isl_pw_aff *SCEVAffinator::visitUMaxExpr(const SCEVUMaxExpr *Expr) {
  llvm_unreachable("SCEVUMaxExpr not yet supported");
}

__isl_give isl_pw_aff *SCEVAffinator::visitUnknown(const SCEVUnknown *Expr) {
  llvm_unreachable("Unknowns are always parameters");
}

int SCEVAffinator::getLoopDepth(const Loop *L) {
  Loop *outerLoop = S->getRegion().outermostLoopInRegion(const_cast<Loop *>(L));
  assert(outerLoop && "Scop does not contain this loop");
  return L->getLoopDepth() - outerLoop->getLoopDepth();
}

//===----------------------------------------------------------------------===//

MemoryAccess::~MemoryAccess() {
  isl_map_free(AccessRelation);
  isl_map_free(newAccessRelation);
}

static void replace(std::string &str, const std::string &find,
                    const std::string &replace) {
  size_t pos = 0;
  while ((pos = str.find(find, pos)) != std::string::npos) {
    str.replace(pos, find.length(), replace);
    pos += replace.length();
  }
}

static void makeIslCompatible(std::string &str) {
  str.erase(0, 1);
  replace(str, ".", "_");
  replace(str, "\"", "_");
}

void MemoryAccess::setBaseName() {
  raw_string_ostream OS(BaseName);
  getBaseAddr()->printAsOperand(OS, false);
  BaseName = OS.str();

  makeIslCompatible(BaseName);
  BaseName = "MemRef_" + BaseName;
}

isl_map *MemoryAccess::getAccessRelation() const {
  return isl_map_copy(AccessRelation);
}

std::string MemoryAccess::getAccessRelationStr() const {
  return stringFromIslObj(AccessRelation);
}

isl_map *MemoryAccess::getNewAccessRelation() const {
  return isl_map_copy(newAccessRelation);
}

isl_basic_map *MemoryAccess::createBasicAccessMap(ScopStmt *Statement) {
  isl_space *Space = isl_space_set_alloc(Statement->getIslCtx(), 0, 1);
  Space = isl_space_set_tuple_name(Space, isl_dim_set, getBaseName().c_str());
  Space = isl_space_align_params(Space, Statement->getDomainSpace());

  return isl_basic_map_from_domain_and_range(
      isl_basic_set_universe(Statement->getDomainSpace()),
      isl_basic_set_universe(Space));
}

MemoryAccess::MemoryAccess(const IRAccess &Access, const Instruction *AccInst,
                           ScopStmt *Statement)
    : Statement(Statement), Inst(AccInst), newAccessRelation(NULL) {

  BaseAddr = Access.getBase();
  setBaseName();

  if (!Access.isAffine()) {
    // We overapproximate non-affine accesses with a possible access to the
    // whole array. For read accesses it does not make a difference, if an
    // access must or may happen. However, for write accesses it is important to
    // differentiate between writes that must happen and writes that may happen.
    AccessRelation = isl_map_from_basic_map(createBasicAccessMap(Statement));
    Type = Access.isRead() ? READ : MAY_WRITE;
    return;
  }

  Type = Access.isRead() ? READ : MUST_WRITE;

  isl_space *Space = isl_space_alloc(Statement->getIslCtx(), 0,
                                     Statement->getNumIterators(), 0);
  AccessRelation = isl_map_universe(Space);

  for (int i = 0, Size = Access.Subscripts.size(); i < Size; ++i) {
    isl_pw_aff *Affine =
        SCEVAffinator::getPwAff(Statement, Access.Subscripts[i]);

    if (i == Size - 1) {
      // Divide the access function of the last subscript by the size of the
      // elements in the array.
      //
      // A stride one array access in C expressed as A[i] is expressed in
      // LLVM-IR as something like A[i * elementsize]. This hides the fact that
      // two subsequent values of 'i' index two values that are stored next to
      // each other in memory. By this division we make this characteristic
      // obvious again.
      isl_val *v;
      v = isl_val_int_from_si(isl_pw_aff_get_ctx(Affine),
                              Access.getElemSizeInBytes());
      Affine = isl_pw_aff_scale_down_val(Affine, v);
    }

    isl_map *SubscriptMap = isl_map_from_pw_aff(Affine);

    AccessRelation = isl_map_flat_range_product(AccessRelation, SubscriptMap);
  }

  Space = Statement->getDomainSpace();
  AccessRelation = isl_map_set_tuple_id(
      AccessRelation, isl_dim_in, isl_space_get_tuple_id(Space, isl_dim_set));
  isl_space_free(Space);
  AccessRelation = isl_map_set_tuple_name(AccessRelation, isl_dim_out,
                                          getBaseName().c_str());
}

void MemoryAccess::realignParams() {
  isl_space *ParamSpace = Statement->getParent()->getParamSpace();
  AccessRelation = isl_map_align_params(AccessRelation, ParamSpace);
}

MemoryAccess::MemoryAccess(const Value *BaseAddress, ScopStmt *Statement)
    : Type(READ), BaseAddr(BaseAddress), Statement(Statement),
      newAccessRelation(nullptr) {

  isl_basic_map *BasicAccessMap = createBasicAccessMap(Statement);
  AccessRelation = isl_map_from_basic_map(BasicAccessMap);
  isl_space *ParamSpace = Statement->getParent()->getParamSpace();
  AccessRelation = isl_map_align_params(AccessRelation, ParamSpace);
}

void MemoryAccess::print(raw_ostream &OS) const {
  switch (Type) {
  case READ:
    OS.indent(12) << "ReadAccess := \n";
    break;
  case MUST_WRITE:
    OS.indent(12) << "MustWriteAccess := \n";
    break;
  case MAY_WRITE:
    OS.indent(12) << "MayWriteAccess := \n";
    break;
  }
  OS.indent(16) << getAccessRelationStr() << ";\n";
}

void MemoryAccess::dump() const { print(errs()); }

// Create a map in the size of the provided set domain, that maps from the
// one element of the provided set domain to another element of the provided
// set domain.
// The mapping is limited to all points that are equal in all but the last
// dimension and for which the last dimension of the input is strict smaller
// than the last dimension of the output.
//
//   getEqualAndLarger(set[i0, i1, ..., iX]):
//
//   set[i0, i1, ..., iX] -> set[o0, o1, ..., oX]
//     : i0 = o0, i1 = o1, ..., i(X-1) = o(X-1), iX < oX
//
static isl_map *getEqualAndLarger(isl_space *setDomain) {
  isl_space *Space = isl_space_map_from_set(setDomain);
  isl_map *Map = isl_map_universe(isl_space_copy(Space));
  isl_local_space *MapLocalSpace = isl_local_space_from_space(Space);
  unsigned lastDimension = isl_map_dim(Map, isl_dim_in) - 1;

  // Set all but the last dimension to be equal for the input and output
  //
  //   input[i0, i1, ..., iX] -> output[o0, o1, ..., oX]
  //     : i0 = o0, i1 = o1, ..., i(X-1) = o(X-1)
  for (unsigned i = 0; i < lastDimension; ++i)
    Map = isl_map_equate(Map, isl_dim_in, i, isl_dim_out, i);

  // Set the last dimension of the input to be strict smaller than the
  // last dimension of the output.
  //
  //   input[?,?,?,...,iX] -> output[?,?,?,...,oX] : iX < oX
  //
  isl_val *v;
  isl_ctx *Ctx = isl_map_get_ctx(Map);
  isl_constraint *c = isl_inequality_alloc(isl_local_space_copy(MapLocalSpace));
  v = isl_val_int_from_si(Ctx, -1);
  c = isl_constraint_set_coefficient_val(c, isl_dim_in, lastDimension, v);
  v = isl_val_int_from_si(Ctx, 1);
  c = isl_constraint_set_coefficient_val(c, isl_dim_out, lastDimension, v);
  v = isl_val_int_from_si(Ctx, -1);
  c = isl_constraint_set_constant_val(c, v);

  Map = isl_map_add_constraint(Map, c);

  isl_local_space_free(MapLocalSpace);
  return Map;
}

isl_set *MemoryAccess::getStride(__isl_take const isl_map *Schedule) const {
  isl_map *S = const_cast<isl_map *>(Schedule);
  isl_map *AccessRelation = getAccessRelation();
  isl_space *Space = isl_space_range(isl_map_get_space(S));
  isl_map *NextScatt = getEqualAndLarger(Space);

  S = isl_map_reverse(S);
  NextScatt = isl_map_lexmin(NextScatt);

  NextScatt = isl_map_apply_range(NextScatt, isl_map_copy(S));
  NextScatt = isl_map_apply_range(NextScatt, isl_map_copy(AccessRelation));
  NextScatt = isl_map_apply_domain(NextScatt, S);
  NextScatt = isl_map_apply_domain(NextScatt, AccessRelation);

  isl_set *Deltas = isl_map_deltas(NextScatt);
  return Deltas;
}

bool MemoryAccess::isStrideX(__isl_take const isl_map *Schedule,
                             int StrideWidth) const {
  isl_set *Stride, *StrideX;
  bool IsStrideX;

  Stride = getStride(Schedule);
  StrideX = isl_set_universe(isl_set_get_space(Stride));
  StrideX = isl_set_fix_si(StrideX, isl_dim_set, 0, StrideWidth);
  IsStrideX = isl_set_is_equal(Stride, StrideX);

  isl_set_free(StrideX);
  isl_set_free(Stride);

  return IsStrideX;
}

bool MemoryAccess::isStrideZero(const isl_map *Schedule) const {
  return isStrideX(Schedule, 0);
}

bool MemoryAccess::isScalar() const {
  return isl_map_n_out(AccessRelation) == 0;
}

bool MemoryAccess::isStrideOne(const isl_map *Schedule) const {
  return isStrideX(Schedule, 1);
}

void MemoryAccess::setNewAccessRelation(isl_map *newAccess) {
  isl_map_free(newAccessRelation);
  newAccessRelation = newAccess;
}

//===----------------------------------------------------------------------===//

isl_map *ScopStmt::getScattering() const { return isl_map_copy(Scattering); }

void ScopStmt::restrictDomain(__isl_take isl_set *NewDomain) {
  assert(isl_set_is_subset(NewDomain, Domain) &&
         "New domain is not a subset of old domain!");
  isl_set_free(Domain);
  Domain = NewDomain;
  Scattering = isl_map_intersect_domain(Scattering, isl_set_copy(Domain));
}

void ScopStmt::setScattering(isl_map *NewScattering) {
  assert(NewScattering && "New scattering is NULL");
  isl_map_free(Scattering);
  Scattering = NewScattering;
}

void ScopStmt::buildScattering(SmallVectorImpl<unsigned> &Scatter) {
  unsigned NbIterators = getNumIterators();
  unsigned NbScatteringDims = Parent.getMaxLoopDepth() * 2 + 1;

  isl_space *Space = isl_space_set_alloc(getIslCtx(), 0, NbScatteringDims);
  Space = isl_space_set_tuple_name(Space, isl_dim_out, "scattering");

  Scattering = isl_map_from_domain_and_range(isl_set_universe(getDomainSpace()),
                                             isl_set_universe(Space));

  // Loop dimensions.
  for (unsigned i = 0; i < NbIterators; ++i)
    Scattering =
        isl_map_equate(Scattering, isl_dim_out, 2 * i + 1, isl_dim_in, i);

  // Constant dimensions
  for (unsigned i = 0; i < NbIterators + 1; ++i)
    Scattering = isl_map_fix_si(Scattering, isl_dim_out, 2 * i, Scatter[i]);

  // Fill scattering dimensions.
  for (unsigned i = 2 * NbIterators + 1; i < NbScatteringDims; ++i)
    Scattering = isl_map_fix_si(Scattering, isl_dim_out, i, 0);

  Scattering = isl_map_align_params(Scattering, Parent.getParamSpace());
}

void ScopStmt::buildAccesses(TempScop &tempScop, const Region &CurRegion) {
  const AccFuncSetType *AccFuncs = tempScop.getAccessFunctions(BB);

  for (AccFuncSetType::const_iterator I = AccFuncs->begin(),
                                      E = AccFuncs->end();
       I != E; ++I) {
    MemAccs.push_back(new MemoryAccess(I->first, I->second, this));

    // We do not track locations for scalar memory accesses at the moment.
    //
    // We do not have a use for this information at the moment. If we need this
    // at some point, the "instruction -> access" mapping needs to be enhanced
    // as a single instruction could then possibly perform multiple accesses.
    if (!I->first.isScalar()) {
      assert(!InstructionToAccess.count(I->second) &&
             "Unexpected 1-to-N mapping on instruction to access map!");
      InstructionToAccess[I->second] = MemAccs.back();
    }
  }
}

void ScopStmt::realignParams() {
  for (memacc_iterator MI = memacc_begin(), ME = memacc_end(); MI != ME; ++MI)
    (*MI)->realignParams();

  Domain = isl_set_align_params(Domain, Parent.getParamSpace());
  Scattering = isl_map_align_params(Scattering, Parent.getParamSpace());
}

__isl_give isl_set *ScopStmt::buildConditionSet(const Comparison &Comp) {
  isl_pw_aff *L = SCEVAffinator::getPwAff(this, Comp.getLHS());
  isl_pw_aff *R = SCEVAffinator::getPwAff(this, Comp.getRHS());

  switch (Comp.getPred()) {
  case ICmpInst::ICMP_EQ:
    return isl_pw_aff_eq_set(L, R);
  case ICmpInst::ICMP_NE:
    return isl_pw_aff_ne_set(L, R);
  case ICmpInst::ICMP_SLT:
    return isl_pw_aff_lt_set(L, R);
  case ICmpInst::ICMP_SLE:
    return isl_pw_aff_le_set(L, R);
  case ICmpInst::ICMP_SGT:
    return isl_pw_aff_gt_set(L, R);
  case ICmpInst::ICMP_SGE:
    return isl_pw_aff_ge_set(L, R);
  case ICmpInst::ICMP_ULT:
  case ICmpInst::ICMP_UGT:
  case ICmpInst::ICMP_ULE:
  case ICmpInst::ICMP_UGE:
    llvm_unreachable("Unsigned comparisons not yet supported");
  default:
    llvm_unreachable("Non integer predicate not supported");
  }
}

__isl_give isl_set *ScopStmt::addLoopBoundsToDomain(__isl_take isl_set *Domain,
                                                    TempScop &tempScop) {
  isl_space *Space;
  isl_local_space *LocalSpace;

  Space = isl_set_get_space(Domain);
  LocalSpace = isl_local_space_from_space(Space);

  for (int i = 0, e = getNumIterators(); i != e; ++i) {
    isl_aff *Zero = isl_aff_zero_on_domain(isl_local_space_copy(LocalSpace));
    isl_pw_aff *IV =
        isl_pw_aff_from_aff(isl_aff_set_coefficient_si(Zero, isl_dim_in, i, 1));

    // 0 <= IV.
    isl_set *LowerBound = isl_pw_aff_nonneg_set(isl_pw_aff_copy(IV));
    Domain = isl_set_intersect(Domain, LowerBound);

    // IV <= LatchExecutions.
    const Loop *L = getLoopForDimension(i);
    const SCEV *LatchExecutions = tempScop.getLoopBound(L);
    isl_pw_aff *UpperBound = SCEVAffinator::getPwAff(this, LatchExecutions);
    isl_set *UpperBoundSet = isl_pw_aff_le_set(IV, UpperBound);
    Domain = isl_set_intersect(Domain, UpperBoundSet);
  }

  isl_local_space_free(LocalSpace);
  return Domain;
}

__isl_give isl_set *ScopStmt::addConditionsToDomain(__isl_take isl_set *Domain,
                                                    TempScop &tempScop,
                                                    const Region &CurRegion) {
  const Region *TopRegion = tempScop.getMaxRegion().getParent(),
               *CurrentRegion = &CurRegion;
  const BasicBlock *BranchingBB = BB;

  do {
    if (BranchingBB != CurrentRegion->getEntry()) {
      if (const BBCond *Condition = tempScop.getBBCond(BranchingBB))
        for (BBCond::const_iterator CI = Condition->begin(),
                                    CE = Condition->end();
             CI != CE; ++CI) {
          isl_set *ConditionSet = buildConditionSet(*CI);
          Domain = isl_set_intersect(Domain, ConditionSet);
        }
    }
    BranchingBB = CurrentRegion->getEntry();
    CurrentRegion = CurrentRegion->getParent();
  } while (TopRegion != CurrentRegion);

  return Domain;
}

__isl_give isl_set *ScopStmt::buildDomain(TempScop &tempScop,
                                          const Region &CurRegion) {
  isl_space *Space;
  isl_set *Domain;
  isl_id *Id;

  Space = isl_space_set_alloc(getIslCtx(), 0, getNumIterators());

  Id = isl_id_alloc(getIslCtx(), getBaseName(), this);

  Domain = isl_set_universe(Space);
  Domain = addLoopBoundsToDomain(Domain, tempScop);
  Domain = addConditionsToDomain(Domain, tempScop, CurRegion);
  Domain = isl_set_set_tuple_id(Domain, Id);

  return Domain;
}

ScopStmt::ScopStmt(Scop &parent, TempScop &tempScop, const Region &CurRegion,
                   BasicBlock &bb, SmallVectorImpl<Loop *> &Nest,
                   SmallVectorImpl<unsigned> &Scatter)
    : Parent(parent), BB(&bb), IVS(Nest.size()), NestLoops(Nest.size()) {
  // Setup the induction variables.
  for (unsigned i = 0, e = Nest.size(); i < e; ++i) {
    if (!SCEVCodegen) {
      PHINode *PN = Nest[i]->getCanonicalInductionVariable();
      assert(PN && "Non canonical IV in Scop!");
      IVS[i] = PN;
    }
    NestLoops[i] = Nest[i];
  }

  raw_string_ostream OS(BaseName);
  bb.printAsOperand(OS, false);
  BaseName = OS.str();

  makeIslCompatible(BaseName);
  BaseName = "Stmt_" + BaseName;

  Domain = buildDomain(tempScop, CurRegion);
  buildScattering(Scatter);
  buildAccesses(tempScop, CurRegion);
}

std::string ScopStmt::getDomainStr() const { return stringFromIslObj(Domain); }

std::string ScopStmt::getScatteringStr() const {
  return stringFromIslObj(Scattering);
}

unsigned ScopStmt::getNumParams() const { return Parent.getNumParams(); }

unsigned ScopStmt::getNumIterators() const {
  // The final read has one dimension with one element.
  if (!BB)
    return 1;

  return NestLoops.size();
}

unsigned ScopStmt::getNumScattering() const {
  return isl_map_dim(Scattering, isl_dim_out);
}

const char *ScopStmt::getBaseName() const { return BaseName.c_str(); }

const PHINode *
ScopStmt::getInductionVariableForDimension(unsigned Dimension) const {
  return IVS[Dimension];
}

const Loop *ScopStmt::getLoopForDimension(unsigned Dimension) const {
  return NestLoops[Dimension];
}

isl_ctx *ScopStmt::getIslCtx() const { return Parent.getIslCtx(); }

isl_set *ScopStmt::getDomain() const { return isl_set_copy(Domain); }

isl_space *ScopStmt::getDomainSpace() const {
  return isl_set_get_space(Domain);
}

isl_id *ScopStmt::getDomainId() const { return isl_set_get_tuple_id(Domain); }

ScopStmt::~ScopStmt() {
  while (!MemAccs.empty()) {
    delete MemAccs.back();
    MemAccs.pop_back();
  }

  isl_set_free(Domain);
  isl_map_free(Scattering);
}

void ScopStmt::print(raw_ostream &OS) const {
  OS << "\t" << getBaseName() << "\n";

  OS.indent(12) << "Domain :=\n";

  if (Domain) {
    OS.indent(16) << getDomainStr() << ";\n";
  } else
    OS.indent(16) << "n/a\n";

  OS.indent(12) << "Scattering :=\n";

  if (Domain) {
    OS.indent(16) << getScatteringStr() << ";\n";
  } else
    OS.indent(16) << "n/a\n";

  for (MemoryAccessVec::const_iterator I = MemAccs.begin(), E = MemAccs.end();
       I != E; ++I)
    (*I)->print(OS);
}

void ScopStmt::dump() const { print(dbgs()); }

//===----------------------------------------------------------------------===//
/// Scop class implement

void Scop::setContext(__isl_take isl_set *NewContext) {
  NewContext = isl_set_align_params(NewContext, isl_set_get_space(Context));
  isl_set_free(Context);
  Context = NewContext;
}

void Scop::addParams(std::vector<const SCEV *> NewParameters) {
  for (std::vector<const SCEV *>::iterator PI = NewParameters.begin(),
                                           PE = NewParameters.end();
       PI != PE; ++PI) {
    const SCEV *Parameter = *PI;

    if (ParameterIds.find(Parameter) != ParameterIds.end())
      continue;

    int dimension = Parameters.size();

    Parameters.push_back(Parameter);
    ParameterIds[Parameter] = dimension;
  }
}

__isl_give isl_id *Scop::getIdForParam(const SCEV *Parameter) const {
  ParamIdType::const_iterator IdIter = ParameterIds.find(Parameter);

  if (IdIter == ParameterIds.end())
    return NULL;

  std::string ParameterName;

  if (const SCEVUnknown *ValueParameter = dyn_cast<SCEVUnknown>(Parameter)) {
    Value *Val = ValueParameter->getValue();
    ParameterName = Val->getName();
  }

  if (ParameterName == "" || ParameterName.substr(0, 2) == "p_")
    ParameterName = "p_" + utostr_32(IdIter->second);

  return isl_id_alloc(getIslCtx(), ParameterName.c_str(), (void *)Parameter);
}

void Scop::buildContext() {
  isl_space *Space = isl_space_params_alloc(IslCtx, 0);
  Context = isl_set_universe(isl_space_copy(Space));
  AssumedContext = isl_set_universe(Space);
}

void Scop::addParameterBounds() {
  for (unsigned i = 0; i < isl_set_dim(Context, isl_dim_param); ++i) {
    isl_val *V;
    isl_id *Id;
    const SCEV *Scev;
    const IntegerType *T;

    Id = isl_set_get_dim_id(Context, isl_dim_param, i);
    Scev = (const SCEV *)isl_id_get_user(Id);
    T = dyn_cast<IntegerType>(Scev->getType());
    isl_id_free(Id);

    assert(T && "Not an integer type");
    int Width = T->getBitWidth();

    V = isl_val_int_from_si(IslCtx, Width - 1);
    V = isl_val_2exp(V);
    V = isl_val_neg(V);
    Context = isl_set_lower_bound_val(Context, isl_dim_param, i, V);

    V = isl_val_int_from_si(IslCtx, Width - 1);
    V = isl_val_2exp(V);
    V = isl_val_sub_ui(V, 1);
    Context = isl_set_upper_bound_val(Context, isl_dim_param, i, V);
  }
}

void Scop::realignParams() {
  // Add all parameters into a common model.
  isl_space *Space = isl_space_params_alloc(IslCtx, ParameterIds.size());

  for (ParamIdType::iterator PI = ParameterIds.begin(), PE = ParameterIds.end();
       PI != PE; ++PI) {
    const SCEV *Parameter = PI->first;
    isl_id *id = getIdForParam(Parameter);
    Space = isl_space_set_dim_id(Space, isl_dim_param, PI->second, id);
  }

  // Align the parameters of all data structures to the model.
  Context = isl_set_align_params(Context, Space);

  for (iterator I = begin(), E = end(); I != E; ++I)
    (*I)->realignParams();
}

Scop::Scop(TempScop &tempScop, LoopInfo &LI, ScalarEvolution &ScalarEvolution,
           isl_ctx *Context)
    : SE(&ScalarEvolution), R(tempScop.getMaxRegion()),
      MaxLoopDepth(tempScop.getMaxLoopDepth()) {
  IslCtx = Context;
  buildContext();

  SmallVector<Loop *, 8> NestLoops;
  SmallVector<unsigned, 8> Scatter;

  Scatter.assign(MaxLoopDepth + 1, 0);

  // Build the iteration domain, access functions and scattering functions
  // traversing the region tree.
  buildScop(tempScop, getRegion(), NestLoops, Scatter, LI);

  realignParams();
  addParameterBounds();

  assert(NestLoops.empty() && "NestLoops not empty at top level!");
}

Scop::~Scop() {
  isl_set_free(Context);
  isl_set_free(AssumedContext);

  // Free the statements;
  for (iterator I = begin(), E = end(); I != E; ++I)
    delete *I;
}

std::string Scop::getContextStr() const { return stringFromIslObj(Context); }

std::string Scop::getNameStr() const {
  std::string ExitName, EntryName;
  raw_string_ostream ExitStr(ExitName);
  raw_string_ostream EntryStr(EntryName);

  R.getEntry()->printAsOperand(EntryStr, false);
  EntryStr.str();

  if (R.getExit()) {
    R.getExit()->printAsOperand(ExitStr, false);
    ExitStr.str();
  } else
    ExitName = "FunctionExit";

  return EntryName + "---" + ExitName;
}

__isl_give isl_set *Scop::getContext() const { return isl_set_copy(Context); }
__isl_give isl_space *Scop::getParamSpace() const {
  return isl_set_get_space(this->Context);
}

__isl_give isl_set *Scop::getAssumedContext() const {
  return isl_set_copy(AssumedContext);
}

void Scop::printContext(raw_ostream &OS) const {
  OS << "Context:\n";

  if (!Context) {
    OS.indent(4) << "n/a\n\n";
    return;
  }

  OS.indent(4) << getContextStr() << "\n";

  for (ParamVecType::const_iterator PI = Parameters.begin(),
                                    PE = Parameters.end();
       PI != PE; ++PI) {
    const SCEV *Parameter = *PI;
    int Dim = ParameterIds.find(Parameter)->second;

    OS.indent(4) << "p" << Dim << ": " << *Parameter << "\n";
  }
}

void Scop::printStatements(raw_ostream &OS) const {
  OS << "Statements {\n";

  for (const_iterator SI = begin(), SE = end(); SI != SE; ++SI)
    OS.indent(4) << (**SI);

  OS.indent(4) << "}\n";
}

void Scop::print(raw_ostream &OS) const {
  OS.indent(4) << "Function: " << getRegion().getEntry()->getParent()->getName()
               << "\n";
  OS.indent(4) << "Region: " << getNameStr() << "\n";
  printContext(OS.indent(4));
  printStatements(OS.indent(4));
}

void Scop::dump() const { print(dbgs()); }

isl_ctx *Scop::getIslCtx() const { return IslCtx; }

__isl_give isl_union_set *Scop::getDomains() {
  isl_union_set *Domain = NULL;

  for (Scop::iterator SI = begin(), SE = end(); SI != SE; ++SI)
    if (!Domain)
      Domain = isl_union_set_from_set((*SI)->getDomain());
    else
      Domain = isl_union_set_union(Domain,
                                   isl_union_set_from_set((*SI)->getDomain()));

  return Domain;
}

__isl_give isl_union_map *Scop::getWrites() {
  isl_union_map *Write = isl_union_map_empty(this->getParamSpace());

  for (Scop::iterator SI = this->begin(), SE = this->end(); SI != SE; ++SI) {
    ScopStmt *Stmt = *SI;

    for (ScopStmt::memacc_iterator MI = Stmt->memacc_begin(),
                                   ME = Stmt->memacc_end();
         MI != ME; ++MI) {
      if (!(*MI)->isWrite())
        continue;

      isl_set *Domain = Stmt->getDomain();
      isl_map *AccessDomain = (*MI)->getAccessRelation();

      AccessDomain = isl_map_intersect_domain(AccessDomain, Domain);
      Write = isl_union_map_add_map(Write, AccessDomain);
    }
  }
  return isl_union_map_coalesce(Write);
}

__isl_give isl_union_map *Scop::getReads() {
  isl_union_map *Read = isl_union_map_empty(this->getParamSpace());

  for (Scop::iterator SI = this->begin(), SE = this->end(); SI != SE; ++SI) {
    ScopStmt *Stmt = *SI;

    for (ScopStmt::memacc_iterator MI = Stmt->memacc_begin(),
                                   ME = Stmt->memacc_end();
         MI != ME; ++MI) {
      if (!(*MI)->isRead())
        continue;

      isl_set *Domain = Stmt->getDomain();
      isl_map *AccessDomain = (*MI)->getAccessRelation();

      AccessDomain = isl_map_intersect_domain(AccessDomain, Domain);
      Read = isl_union_map_add_map(Read, AccessDomain);
    }
  }
  return isl_union_map_coalesce(Read);
}

__isl_give isl_union_map *Scop::getSchedule() {
  isl_union_map *Schedule = isl_union_map_empty(this->getParamSpace());

  for (Scop::iterator SI = this->begin(), SE = this->end(); SI != SE; ++SI) {
    ScopStmt *Stmt = *SI;
    Schedule = isl_union_map_add_map(Schedule, Stmt->getScattering());
  }
  return isl_union_map_coalesce(Schedule);
}

bool Scop::restrictDomains(__isl_take isl_union_set *Domain) {
  bool Changed = false;
  for (Scop::iterator SI = this->begin(), SE = this->end(); SI != SE; ++SI) {
    ScopStmt *Stmt = *SI;
    isl_union_set *StmtDomain = isl_union_set_from_set(Stmt->getDomain());

    isl_union_set *NewStmtDomain = isl_union_set_intersect(
        isl_union_set_copy(StmtDomain), isl_union_set_copy(Domain));

    if (isl_union_set_is_subset(StmtDomain, NewStmtDomain)) {
      isl_union_set_free(StmtDomain);
      isl_union_set_free(NewStmtDomain);
      continue;
    }

    Changed = true;

    isl_union_set_free(StmtDomain);
    NewStmtDomain = isl_union_set_coalesce(NewStmtDomain);

    if (isl_union_set_is_empty(NewStmtDomain)) {
      Stmt->restrictDomain(isl_set_empty(Stmt->getDomainSpace()));
      isl_union_set_free(NewStmtDomain);
    } else
      Stmt->restrictDomain(isl_set_from_union_set(NewStmtDomain));
  }
  isl_union_set_free(Domain);
  return Changed;
}

ScalarEvolution *Scop::getSE() const { return SE; }

bool Scop::isTrivialBB(BasicBlock *BB, TempScop &tempScop) {
  if (tempScop.getAccessFunctions(BB))
    return false;

  return true;
}

void Scop::buildScop(TempScop &tempScop, const Region &CurRegion,
                     SmallVectorImpl<Loop *> &NestLoops,
                     SmallVectorImpl<unsigned> &Scatter, LoopInfo &LI) {
  Loop *L = castToLoop(CurRegion, LI);

  if (L)
    NestLoops.push_back(L);

  unsigned loopDepth = NestLoops.size();
  assert(Scatter.size() > loopDepth && "Scatter not big enough!");

  for (Region::const_element_iterator I = CurRegion.element_begin(),
                                      E = CurRegion.element_end();
       I != E; ++I)
    if (I->isSubRegion())
      buildScop(tempScop, *(I->getNodeAs<Region>()), NestLoops, Scatter, LI);
    else {
      BasicBlock *BB = I->getNodeAs<BasicBlock>();

      if (isTrivialBB(BB, tempScop))
        continue;

      Stmts.push_back(
          new ScopStmt(*this, tempScop, CurRegion, *BB, NestLoops, Scatter));

      // Increasing the Scattering function is OK for the moment, because
      // we are using a depth first iterator and the program is well structured.
      ++Scatter[loopDepth];
    }

  if (!L)
    return;

  // Exiting a loop region.
  Scatter[loopDepth] = 0;
  NestLoops.pop_back();
  ++Scatter[loopDepth - 1];
}

//===----------------------------------------------------------------------===//
ScopInfo::ScopInfo() : RegionPass(ID), scop(0) {
  ctx = isl_ctx_alloc();
  isl_options_set_on_error(ctx, ISL_ON_ERROR_ABORT);
}

ScopInfo::~ScopInfo() {
  clear();
  isl_ctx_free(ctx);
}

void ScopInfo::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfo>();
  AU.addRequired<RegionInfo>();
  AU.addRequired<ScalarEvolution>();
  AU.addRequired<TempScopInfo>();
  AU.setPreservesAll();
}

bool ScopInfo::runOnRegion(Region *R, RGPassManager &RGM) {
  LoopInfo &LI = getAnalysis<LoopInfo>();
  ScalarEvolution &SE = getAnalysis<ScalarEvolution>();

  TempScop *tempScop = getAnalysis<TempScopInfo>().getTempScop(R);

  // This region is no Scop.
  if (!tempScop) {
    scop = 0;
    return false;
  }

  // Statistics.
  ++ScopFound;
  if (tempScop->getMaxLoopDepth() > 0)
    ++RichScopFound;

  scop = new Scop(*tempScop, LI, SE, ctx);

  return false;
}

char ScopInfo::ID = 0;

Pass *polly::createScopInfoPass() { return new ScopInfo(); }

INITIALIZE_PASS_BEGIN(ScopInfo, "polly-scops",
                      "Polly - Create polyhedral description of Scops", false,
                      false);
INITIALIZE_PASS_DEPENDENCY(LoopInfo);
INITIALIZE_PASS_DEPENDENCY(RegionInfo);
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution);
INITIALIZE_PASS_DEPENDENCY(TempScopInfo);
INITIALIZE_PASS_END(ScopInfo, "polly-scops",
                    "Polly - Create polyhedral description of Scops", false,
                    false)
