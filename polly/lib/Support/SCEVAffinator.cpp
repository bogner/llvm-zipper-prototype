//===--------- SCEVAffinator.cpp  - Create Scops from LLVM IR -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Create a polyhedral description for a SCEV value.
//
//===----------------------------------------------------------------------===//

#include "polly/Support/SCEVAffinator.h"
#include "polly/Options.h"
#include "polly/ScopInfo.h"
#include "polly/Support/GICHelper.h"
#include "polly/Support/SCEVValidator.h"
#include "polly/Support/ScopHelper.h"
#include "isl/aff.h"
#include "isl/local_space.h"
#include "isl/set.h"
#include "isl/val.h"

using namespace llvm;
using namespace polly;

static cl::opt<bool> IgnoreIntegerWrapping(
    "polly-ignore-integer-wrapping",
    cl::desc("Do not build run-time checks to proof absence of integer "
             "wrapping"),
    cl::Hidden, cl::ZeroOrMore, cl::init(false), cl::cat(PollyCategory));

// The maximal number of basic sets we allow during the construction of a
// piecewise affine function. More complex ones will result in very high
// compile time.
static int const MaxConjunctsInPwAff = 100;

/// @brief Add the number of basic sets in @p Domain to @p User
static isl_stat addNumBasicSets(isl_set *Domain, isl_aff *Aff, void *User) {
  auto *NumBasicSets = static_cast<unsigned *>(User);
  *NumBasicSets += isl_set_n_basic_set(Domain);
  isl_set_free(Domain);
  isl_aff_free(Aff);
  return isl_stat_ok;
}

/// @brief Determine if @p PWA is to complex to continue
///
/// Note that @p PWA will be "free" (deallocated) if this function returns true,
/// but not if this function returns false.
static bool isToComplex(isl_pw_aff *PWA) {
  unsigned NumBasicSets = 0;
  isl_pw_aff_foreach_piece(PWA, addNumBasicSets, &NumBasicSets);
  if (NumBasicSets <= MaxConjunctsInPwAff)
    return false;
  isl_pw_aff_free(PWA);
  return true;
}

/// @brief Return the flag describing the possible wrapping of @p Expr.
static SCEV::NoWrapFlags getNoWrapFlags(const SCEV *Expr) {
  if (auto *NAry = dyn_cast<SCEVNAryExpr>(Expr))
    return NAry->getNoWrapFlags();
  return SCEV::NoWrapMask;
}

SCEVAffinator::SCEVAffinator(Scop *S, LoopInfo &LI)
    : S(S), Ctx(S->getIslCtx()), R(S->getRegion()), SE(*S->getSE()), LI(LI),
      TD(R.getEntry()->getParent()->getParent()->getDataLayout()) {}

SCEVAffinator::~SCEVAffinator() {
  for (const auto &CachedPair : CachedExpressions)
    isl_pw_aff_free(CachedPair.second);
}

__isl_give isl_pw_aff *SCEVAffinator::getPwAff(const SCEV *Expr,
                                               BasicBlock *BB) {
  this->BB = BB;

  if (BB) {
    auto *DC = S->getDomainConditions(BB);
    NumIterators = isl_set_n_dim(DC);
    isl_set_free(DC);
  } else
    NumIterators = 0;

  auto *Scope = LI.getLoopFor(BB);
  S->addParams(getParamsInAffineExpr(&R, Scope, Expr, SE));

  return visit(Expr);
}

void SCEVAffinator::checkForWrapping(const SCEV *Expr,
                                     __isl_keep isl_pw_aff *PWA) const {
  // If the SCEV flags do contain NSW (no signed wrap) then PWA already
  // represents Expr in modulo semantic (it is not allowed to overflow), thus we
  // are done. Otherwise, we will compute:
  //   PWA = ((PWA + 2^(n-1)) mod (2 ^ n)) - 2^(n-1)
  // whereas n is the number of bits of the Expr, hence:
  //   n = bitwidth(ExprType)

  if (IgnoreIntegerWrapping || (getNoWrapFlags(Expr) & SCEV::FlagNSW))
    return;

  auto *PWAMod = addModuloSemantic(isl_pw_aff_copy(PWA), Expr->getType());
  auto *NotEqualSet = isl_pw_aff_ne_set(isl_pw_aff_copy(PWA), PWAMod);

  const DebugLoc &Loc = BB ? BB->getTerminator()->getDebugLoc() : DebugLoc();
  NotEqualSet = BB ? NotEqualSet : isl_set_params(NotEqualSet);

  if (isl_set_is_empty(NotEqualSet))
    isl_set_free(NotEqualSet);
  else
    S->recordAssumption(WRAPPING, NotEqualSet, Loc, AS_RESTRICTION, BB);
}

__isl_give isl_pw_aff *
SCEVAffinator::addModuloSemantic(__isl_take isl_pw_aff *PWA,
                                 Type *ExprType) const {
  unsigned Width = TD.getTypeStoreSizeInBits(ExprType);
  isl_ctx *Ctx = isl_pw_aff_get_ctx(PWA);

  isl_val *ModVal = isl_val_int_from_ui(Ctx, Width);
  ModVal = isl_val_2exp(ModVal);

  isl_val *AddVal = isl_val_int_from_ui(Ctx, Width - 1);
  AddVal = isl_val_2exp(AddVal);

  isl_set *Domain = isl_pw_aff_domain(isl_pw_aff_copy(PWA));

  isl_pw_aff *AddPW = isl_pw_aff_val_on_domain(Domain, AddVal);

  PWA = isl_pw_aff_add(PWA, isl_pw_aff_copy(AddPW));
  PWA = isl_pw_aff_mod_val(PWA, ModVal);
  PWA = isl_pw_aff_sub(PWA, AddPW);

  return PWA;
}

bool SCEVAffinator::hasNSWAddRecForLoop(Loop *L) const {
  for (const auto &CachedPair : CachedExpressions) {
    auto *AddRec = dyn_cast<SCEVAddRecExpr>(CachedPair.first.first);
    if (!AddRec)
      continue;
    if (AddRec->getLoop() != L)
      continue;
    if (AddRec->getNoWrapFlags() & SCEV::FlagNSW)
      return true;
  }

  return false;
}

__isl_give isl_pw_aff *SCEVAffinator::visit(const SCEV *Expr) {

  auto Key = std::make_pair(Expr, BB);
  isl_pw_aff *PWA = CachedExpressions[Key];
  if (PWA)
    return isl_pw_aff_copy(PWA);

  auto ConstantAndLeftOverPair = extractConstantFactor(Expr, *S->getSE());
  auto *Factor = ConstantAndLeftOverPair.first;
  Expr = ConstantAndLeftOverPair.second;

  // In case the scev is a valid parameter, we do not further analyze this
  // expression, but create a new parameter in the isl_pw_aff. This allows us
  // to treat subexpressions that we cannot translate into an piecewise affine
  // expression, as constant parameters of the piecewise affine expression.
  if (isl_id *Id = S->getIdForParam(Expr)) {
    isl_space *Space = isl_space_set_alloc(Ctx, 1, NumIterators);
    Space = isl_space_set_dim_id(Space, isl_dim_param, 0, Id);

    isl_set *Domain = isl_set_universe(isl_space_copy(Space));
    isl_aff *Affine = isl_aff_zero_on_domain(isl_local_space_from_space(Space));
    Affine = isl_aff_add_coefficient_si(Affine, isl_dim_param, 0, 1);

    PWA = isl_pw_aff_alloc(Domain, Affine);
  } else {
    PWA = SCEVVisitor<SCEVAffinator, isl_pw_aff *>::visit(Expr);
    checkForWrapping(Expr, PWA);
  }

  PWA = isl_pw_aff_mul(visitConstant(Factor), PWA);

  // For compile time reasons we need to simplify the PWA before we cache and
  // return it.
  PWA = isl_pw_aff_coalesce(PWA);
  checkForWrapping(Key.first, PWA);

  CachedExpressions[Key] = isl_pw_aff_copy(PWA);
  return PWA;
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

  isl_space *Space = isl_space_set_alloc(Ctx, 0, NumIterators);
  isl_local_space *ls = isl_local_space_from_space(Space);
  return isl_pw_aff_from_aff(isl_aff_val_on_domain(ls, v));
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
    if (isToComplex(Sum))
      return nullptr;
  }

  return Sum;
}

__isl_give isl_pw_aff *SCEVAffinator::visitMulExpr(const SCEVMulExpr *Expr) {
  isl_pw_aff *Prod = visit(Expr->getOperand(0));

  for (int i = 1, e = Expr->getNumOperands(); i < e; ++i) {
    isl_pw_aff *NextFactor = visit(Expr->getOperand(i));
    Prod = isl_pw_aff_mul(Prod, NextFactor);
    if (isToComplex(Prod))
      return nullptr;
  }

  return Prod;
}

__isl_give isl_pw_aff *SCEVAffinator::visitUDivExpr(const SCEVUDivExpr *Expr) {
  llvm_unreachable("SCEVUDivExpr not yet supported");
}

__isl_give isl_pw_aff *
SCEVAffinator::visitAddRecExpr(const SCEVAddRecExpr *Expr) {
  assert(Expr->isAffine() && "Only affine AddRecurrences allowed");

  auto Flags = Expr->getNoWrapFlags();

  // Directly generate isl_pw_aff for Expr if 'start' is zero.
  if (Expr->getStart()->isZero()) {
    assert(S->getRegion().contains(Expr->getLoop()) &&
           "Scop does not contain the loop referenced in this AddRec");

    isl_pw_aff *Step = visit(Expr->getOperand(1));
    isl_space *Space = isl_space_set_alloc(Ctx, 0, NumIterators);
    isl_local_space *LocalSpace = isl_local_space_from_space(Space);

    unsigned loopDimension = S->getRelativeLoopDepth(Expr->getLoop());

    isl_aff *LAff = isl_aff_set_coefficient_si(
        isl_aff_zero_on_domain(LocalSpace), isl_dim_in, loopDimension, 1);
    isl_pw_aff *LPwAff = isl_pw_aff_from_aff(LAff);

    return isl_pw_aff_mul(Step, LPwAff);
  }

  // Translate AddRecExpr from '{start, +, inc}' into 'start + {0, +, inc}'
  // if 'start' is not zero.
  // TODO: Using the original SCEV no-wrap flags is not always safe, however
  //       as our code generation is reordering the expression anyway it doesn't
  //       really matter.
  ScalarEvolution &SE = *S->getSE();
  const SCEV *ZeroStartExpr =
      SE.getAddRecExpr(SE.getConstant(Expr->getStart()->getType(), 0),
                       Expr->getStepRecurrence(SE), Expr->getLoop(), Flags);

  isl_pw_aff *ZeroStartResult = visit(ZeroStartExpr);
  isl_pw_aff *Start = visit(Expr->getStart());

  return isl_pw_aff_add(ZeroStartResult, Start);
}

__isl_give isl_pw_aff *SCEVAffinator::visitSMaxExpr(const SCEVSMaxExpr *Expr) {
  isl_pw_aff *Max = visit(Expr->getOperand(0));

  for (int i = 1, e = Expr->getNumOperands(); i < e; ++i) {
    isl_pw_aff *NextOperand = visit(Expr->getOperand(i));
    Max = isl_pw_aff_max(Max, NextOperand);
    if (isToComplex(Max))
      return nullptr;
  }

  return Max;
}

__isl_give isl_pw_aff *SCEVAffinator::visitUMaxExpr(const SCEVUMaxExpr *Expr) {
  llvm_unreachable("SCEVUMaxExpr not yet supported");
}

__isl_give isl_pw_aff *SCEVAffinator::visitSDivInstruction(Instruction *SDiv) {
  assert(SDiv->getOpcode() == Instruction::SDiv && "Assumed SDiv instruction!");
  auto *SE = S->getSE();

  auto *Divisor = SDiv->getOperand(1);
  auto *DivisorSCEV = SE->getSCEV(Divisor);
  auto *DivisorPWA = visit(DivisorSCEV);
  assert(isa<ConstantInt>(Divisor) &&
         "SDiv is no parameter but has a non-constant RHS.");

  auto *Dividend = SDiv->getOperand(0);
  auto *DividendSCEV = SE->getSCEV(Dividend);
  auto *DividendPWA = visit(DividendSCEV);
  return isl_pw_aff_tdiv_q(DividendPWA, DivisorPWA);
}

__isl_give isl_pw_aff *SCEVAffinator::visitSRemInstruction(Instruction *SRem) {
  assert(SRem->getOpcode() == Instruction::SRem && "Assumed SRem instruction!");
  auto *SE = S->getSE();

  auto *Divisor = dyn_cast<ConstantInt>(SRem->getOperand(1));
  assert(Divisor && "SRem is no parameter but has a non-constant RHS.");
  auto *DivisorVal = isl_valFromAPInt(Ctx, Divisor->getValue(),
                                      /* isSigned */ true);

  auto *Dividend = SRem->getOperand(0);
  auto *DividendSCEV = SE->getSCEV(Dividend);
  auto *DividendPWA = visit(DividendSCEV);

  return isl_pw_aff_mod_val(DividendPWA, isl_val_abs(DivisorVal));
}

__isl_give isl_pw_aff *SCEVAffinator::visitUnknown(const SCEVUnknown *Expr) {
  if (Instruction *I = dyn_cast<Instruction>(Expr->getValue())) {
    switch (I->getOpcode()) {
    case Instruction::SDiv:
      return visitSDivInstruction(I);
    case Instruction::SRem:
      return visitSRemInstruction(I);
    default:
      break; // Fall through.
    }
  }

  llvm_unreachable(
      "Unknowns SCEV was neither parameter nor a valid instruction.");
}
