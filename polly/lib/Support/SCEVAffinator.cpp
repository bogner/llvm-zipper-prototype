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

// The maximal number of bits for which a zero-extend is modeled precisely.
static unsigned const MaxZextSmallBitWidth = 7;

/// @brief Return true if a zero-extend from @p Width bits is precisely modeled.
static bool isPreciseZeroExtend(unsigned Width) {
  return Width <= MaxZextSmallBitWidth;
}

/// @brief Add the number of basic sets in @p Domain to @p User
static isl_stat addNumBasicSets(isl_set *Domain, isl_aff *Aff, void *User) {
  auto *NumBasicSets = static_cast<unsigned *>(User);
  *NumBasicSets += isl_set_n_basic_set(Domain);
  isl_set_free(Domain);
  isl_aff_free(Aff);
  return isl_stat_ok;
}

/// @brief Helper to free a PWACtx object.
static void freePWACtx(__isl_take PWACtx &PWAC) {
  isl_pw_aff_free(PWAC.first);
  isl_set_free(PWAC.second);
}

/// @brief Helper to copy a PWACtx object.
static __isl_give PWACtx copyPWACtx(const __isl_keep PWACtx &PWAC) {
  return std::make_pair(isl_pw_aff_copy(PWAC.first), isl_set_copy(PWAC.second));
}

/// @brief Determine if @p PWAC is too complex to continue.
///
/// Note that @p PWAC will be "free" (deallocated) if this function returns
/// true, but not if this function returns false.
static bool isTooComplex(PWACtx &PWAC) {
  unsigned NumBasicSets = 0;
  isl_pw_aff_foreach_piece(PWAC.first, addNumBasicSets, &NumBasicSets);
  if (NumBasicSets <= MaxConjunctsInPwAff)
    return false;
  freePWACtx(PWAC);
  return true;
}

/// @brief Return the flag describing the possible wrapping of @p Expr.
static SCEV::NoWrapFlags getNoWrapFlags(const SCEV *Expr) {
  if (auto *NAry = dyn_cast<SCEVNAryExpr>(Expr))
    return NAry->getNoWrapFlags();
  return SCEV::NoWrapMask;
}

static void combine(__isl_keep PWACtx &PWAC0, const __isl_take PWACtx &PWAC1,
                    isl_pw_aff *(Fn)(isl_pw_aff *, isl_pw_aff *)) {
  PWAC0.first = Fn(PWAC0.first, PWAC1.first);
  PWAC0.second = isl_set_union(PWAC0.second, PWAC1.second);
}

/// @brief Set the possible wrapping of @p Expr to @p Flags.
static const SCEV *setNoWrapFlags(ScalarEvolution &SE, const SCEV *Expr,
                                  SCEV::NoWrapFlags Flags) {
  auto *NAry = dyn_cast<SCEVNAryExpr>(Expr);
  if (!NAry)
    return Expr;

  SmallVector<const SCEV *, 8> Ops(NAry->op_begin(), NAry->op_end());
  switch (Expr->getSCEVType()) {
  case scAddExpr:
    return SE.getAddExpr(Ops, Flags);
  case scMulExpr:
    return SE.getMulExpr(Ops, Flags);
  case scAddRecExpr:
    return SE.getAddRecExpr(Ops, cast<SCEVAddRecExpr>(Expr)->getLoop(), Flags);
  default:
    return Expr;
  }
}

static __isl_give isl_pw_aff *getWidthExpValOnDomain(unsigned Width,
                                                     __isl_take isl_set *Dom) {
  auto *Ctx = isl_set_get_ctx(Dom);
  auto *WidthVal = isl_val_int_from_ui(Ctx, Width);
  auto *ExpVal = isl_val_2exp(WidthVal);
  return isl_pw_aff_val_on_domain(Dom, ExpVal);
}

SCEVAffinator::SCEVAffinator(Scop *S, LoopInfo &LI)
    : S(S), Ctx(S->getIslCtx()), R(S->getRegion()), SE(*S->getSE()), LI(LI),
      TD(R.getEntry()->getParent()->getParent()->getDataLayout()) {}

SCEVAffinator::~SCEVAffinator() {
  for (auto &CachedPair : CachedExpressions)
    freePWACtx(CachedPair.second);
}

void SCEVAffinator::takeNonNegativeAssumption(PWACtx &PWAC) {
  auto *NegPWA = isl_pw_aff_neg(isl_pw_aff_copy(PWAC.first));
  auto *NegDom = isl_pw_aff_pos_set(NegPWA);
  PWAC.second = isl_set_union(PWAC.second, isl_set_copy(NegDom));
  auto *Restriction = BB ? NegDom : isl_set_params(NegDom);
  auto DL = BB ? BB->getTerminator()->getDebugLoc() : DebugLoc();
  S->recordAssumption(UNSIGNED, Restriction, DL, AS_RESTRICTION, BB);
}

__isl_give PWACtx SCEVAffinator::getPWACtxFromPWA(__isl_take isl_pw_aff *PWA) {
  return std::make_pair(
      PWA, isl_set_empty(isl_space_set_alloc(Ctx, 0, NumIterators)));
}

__isl_give PWACtx SCEVAffinator::getPwAff(const SCEV *Expr, BasicBlock *BB) {
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

__isl_give PWACtx SCEVAffinator::checkForWrapping(const SCEV *Expr,
                                                  PWACtx PWAC) const {
  // If the SCEV flags do contain NSW (no signed wrap) then PWA already
  // represents Expr in modulo semantic (it is not allowed to overflow), thus we
  // are done. Otherwise, we will compute:
  //   PWA = ((PWA + 2^(n-1)) mod (2 ^ n)) - 2^(n-1)
  // whereas n is the number of bits of the Expr, hence:
  //   n = bitwidth(ExprType)

  if (IgnoreIntegerWrapping || (getNoWrapFlags(Expr) & SCEV::FlagNSW))
    return PWAC;

  auto *PWA = PWAC.first;
  auto *PWAMod = addModuloSemantic(isl_pw_aff_copy(PWA), Expr->getType());
  auto *NotEqualSet = isl_pw_aff_ne_set(isl_pw_aff_copy(PWA), PWAMod);
  PWAC.second = isl_set_union(PWAC.second, isl_set_copy(NotEqualSet));

  const DebugLoc &Loc = BB ? BB->getTerminator()->getDebugLoc() : DebugLoc();
  NotEqualSet = BB ? NotEqualSet : isl_set_params(NotEqualSet);

  if (isl_set_is_empty(NotEqualSet))
    isl_set_free(NotEqualSet);
  else
    S->recordAssumption(WRAPPING, NotEqualSet, Loc, AS_RESTRICTION, BB);

  return PWAC;
}

__isl_give isl_pw_aff *
SCEVAffinator::addModuloSemantic(__isl_take isl_pw_aff *PWA,
                                 Type *ExprType) const {
  unsigned Width = TD.getTypeSizeInBits(ExprType);
  isl_ctx *Ctx = isl_pw_aff_get_ctx(PWA);

  isl_val *ModVal = isl_val_int_from_ui(Ctx, Width);
  ModVal = isl_val_2exp(ModVal);

  isl_set *Domain = isl_pw_aff_domain(isl_pw_aff_copy(PWA));
  isl_pw_aff *AddPW = getWidthExpValOnDomain(Width - 1, Domain);

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

__isl_give PWACtx SCEVAffinator::visit(const SCEV *Expr) {

  auto Key = std::make_pair(Expr, BB);
  PWACtx PWAC = CachedExpressions[Key];
  if (PWAC.first)
    return copyPWACtx(PWAC);

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

    PWAC = getPWACtxFromPWA(isl_pw_aff_alloc(Domain, Affine));
  } else {
    PWAC = SCEVVisitor<SCEVAffinator, PWACtx>::visit(Expr);
    PWAC = checkForWrapping(Expr, PWAC);
  }

  combine(PWAC, visitConstant(Factor), isl_pw_aff_mul);

  // For compile time reasons we need to simplify the PWAC before we cache and
  // return it.
  PWAC.first = isl_pw_aff_coalesce(PWAC.first);
  PWAC = checkForWrapping(Key.first, PWAC);

  CachedExpressions[Key] = copyPWACtx(PWAC);
  return PWAC;
}

__isl_give PWACtx SCEVAffinator::visitConstant(const SCEVConstant *Expr) {
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
  return getPWACtxFromPWA(isl_pw_aff_from_aff(isl_aff_val_on_domain(ls, v)));
}

__isl_give PWACtx
SCEVAffinator::visitTruncateExpr(const SCEVTruncateExpr *Expr) {
  llvm_unreachable("SCEVTruncateExpr not yet supported");
}

__isl_give PWACtx
SCEVAffinator::visitZeroExtendExpr(const SCEVZeroExtendExpr *Expr) {
  // A zero-extended value can be interpreted as a piecewise defined signed
  // value. If the value was non-negative it stays the same, otherwise it
  // is the sum of the original value and 2^n where n is the bit-width of
  // the original (or operand) type. Examples:
  //   zext i8 127 to i32 -> { [127] }
  //   zext i8  -1 to i32 -> { [256 + (-1)] } = { [255] }
  //   zext i8  %v to i32 -> [v] -> { [v] | v >= 0; [256 + v] | v < 0 }
  //
  // However, LLVM/Scalar Evolution uses zero-extend (potentially lead by a
  // truncate) to represent some forms of modulo computation. The left-hand side
  // of the condition in the code below would result in the SCEV
  // "zext i1 <false, +, true>for.body" which is just another description
  // of the C expression "i & 1 != 0" or, equivalently, "i % 2 != 0".
  //
  //   for (i = 0; i < N; i++)
  //     if (i & 1 != 0 /* == i % 2 */)
  //       /* do something */
  //
  // If we do not make the modulo explicit but only use the mechanism described
  // above we will get the very restrictive assumption "N < 3", because for all
  // values of N >= 3 the SCEVAddRecExpr operand of the zero-extend would wrap.
  // Alternatively, we can make the modulo in the operand explicit in the
  // resulting piecewise function and thereby avoid the assumption on N. For the
  // example this would result in the following piecewise affine function:
  // { [i0] -> [(1)] : 2*floor((-1 + i0)/2) = -1 + i0;
  //   [i0] -> [(0)] : 2*floor((i0)/2) = i0 }
  // To this end we can first determine if the (immediate) operand of the
  // zero-extend can wrap and, in case it might, we will use explicit modulo
  // semantic to compute the result instead of emitting non-wrapping
  // assumptions.
  //
  // Note that operands with large bit-widths are less likely to be negative
  // because it would result in a very large access offset or loop bound after
  // the zero-extend. To this end one can optimistically assume the operand to
  // be positive and avoid the piecewise definition if the bit-width is bigger
  // than some threshold (here MaxZextSmallBitWidth).
  //
  // We choose to go with a hybrid solution of all modeling techniques described
  // above. For small bit-widths (up to MaxZextSmallBitWidth) we will model the
  // wrapping explicitly and use a piecewise defined function. However, if the
  // bit-width is bigger than MaxZextSmallBitWidth we will employ overflow
  // assumptions and assume the "former negative" piece will not exist.

  auto *Op = Expr->getOperand();
  unsigned Width = TD.getTypeSizeInBits(Op->getType());

  bool Precise = isPreciseZeroExtend(Width);

  auto Flags = getNoWrapFlags(Op);
  auto NoWrapFlags = ScalarEvolution::setFlags(Flags, SCEV::FlagNSW);
  bool OpCanWrap = Precise && !(Flags & SCEV::FlagNSW);
  if (OpCanWrap)
    Op = setNoWrapFlags(SE, Op, NoWrapFlags);

  auto OpPWAC = visit(Op);
  if (OpCanWrap)
    OpPWAC.first =
        addModuloSemantic(OpPWAC.first, Expr->getOperand()->getType());

  // If the width is to big we assume the negative part does not occur.
  if (!Precise) {
    takeNonNegativeAssumption(OpPWAC);
    return OpPWAC;
  }

  // If the width is small build the piece for the non-negative part and
  // the one for the negative part and unify them.
  auto *NonNegDom = isl_pw_aff_nonneg_set(isl_pw_aff_copy(OpPWAC.first));
  auto *NonNegPWA = isl_pw_aff_intersect_domain(isl_pw_aff_copy(OpPWAC.first),
                                                isl_set_copy(NonNegDom));
  auto *ExpPWA = getWidthExpValOnDomain(Width, isl_set_complement(NonNegDom));
  OpPWAC.first = isl_pw_aff_add(OpPWAC.first, ExpPWA);
  OpPWAC.first = isl_pw_aff_union_add(NonNegPWA, OpPWAC.first);
  return OpPWAC;
}

__isl_give PWACtx
SCEVAffinator::visitSignExtendExpr(const SCEVSignExtendExpr *Expr) {
  // As all values are represented as signed, a sign extension is a noop.
  return visit(Expr->getOperand());
}

__isl_give PWACtx SCEVAffinator::visitAddExpr(const SCEVAddExpr *Expr) {
  PWACtx Sum = visit(Expr->getOperand(0));

  for (int i = 1, e = Expr->getNumOperands(); i < e; ++i) {
    combine(Sum, visit(Expr->getOperand(i)), isl_pw_aff_add);
    if (isTooComplex(Sum))
      return std::make_pair(nullptr, nullptr);
  }

  return Sum;
}

__isl_give PWACtx SCEVAffinator::visitMulExpr(const SCEVMulExpr *Expr) {
  PWACtx Prod = visit(Expr->getOperand(0));

  for (int i = 1, e = Expr->getNumOperands(); i < e; ++i) {
    combine(Prod, visit(Expr->getOperand(i)), isl_pw_aff_mul);
    if (isTooComplex(Prod))
      return std::make_pair(nullptr, nullptr);
  }

  return Prod;
}

__isl_give PWACtx SCEVAffinator::visitAddRecExpr(const SCEVAddRecExpr *Expr) {
  assert(Expr->isAffine() && "Only affine AddRecurrences allowed");

  auto Flags = Expr->getNoWrapFlags();

  // Directly generate isl_pw_aff for Expr if 'start' is zero.
  if (Expr->getStart()->isZero()) {
    assert(S->getRegion().contains(Expr->getLoop()) &&
           "Scop does not contain the loop referenced in this AddRec");

    PWACtx Step = visit(Expr->getOperand(1));
    isl_space *Space = isl_space_set_alloc(Ctx, 0, NumIterators);
    isl_local_space *LocalSpace = isl_local_space_from_space(Space);

    unsigned loopDimension = S->getRelativeLoopDepth(Expr->getLoop());

    isl_aff *LAff = isl_aff_set_coefficient_si(
        isl_aff_zero_on_domain(LocalSpace), isl_dim_in, loopDimension, 1);
    isl_pw_aff *LPwAff = isl_pw_aff_from_aff(LAff);

    Step.first = isl_pw_aff_mul(Step.first, LPwAff);
    return Step;
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

  PWACtx Result = visit(ZeroStartExpr);
  PWACtx Start = visit(Expr->getStart());
  combine(Result, Start, isl_pw_aff_add);
  return Result;
}

__isl_give PWACtx SCEVAffinator::visitSMaxExpr(const SCEVSMaxExpr *Expr) {
  PWACtx Max = visit(Expr->getOperand(0));

  for (int i = 1, e = Expr->getNumOperands(); i < e; ++i) {
    combine(Max, visit(Expr->getOperand(i)), isl_pw_aff_max);
    if (isTooComplex(Max))
      return std::make_pair(nullptr, nullptr);
  }

  return Max;
}

__isl_give PWACtx SCEVAffinator::visitUMaxExpr(const SCEVUMaxExpr *Expr) {
  llvm_unreachable("SCEVUMaxExpr not yet supported");
}

__isl_give PWACtx SCEVAffinator::visitUDivExpr(const SCEVUDivExpr *Expr) {
  // The handling of unsigned division is basically the same as for signed
  // division, except the interpretation of the operands. As the divisor
  // has to be constant in both cases we can simply interpret it as an
  // unsigned value without additional complexity in the representation.
  // For the dividend we could choose from the different representation
  // schemes introduced for zero-extend operations but for now we will
  // simply use an assumption.
  auto *Dividend = Expr->getLHS();
  auto *Divisor = Expr->getRHS();
  assert(isa<SCEVConstant>(Divisor) &&
         "UDiv is no parameter but has a non-constant RHS.");

  auto DividendPWAC = visit(Dividend);
  auto DivisorPWAC = visit(Divisor);

  if (SE.isKnownNegative(Divisor)) {
    // Interpret negative divisors unsigned. This is a special case of the
    // piece-wise defined value described for zero-extends as we already know
    // the actual value of the constant divisor.
    unsigned Width = TD.getTypeSizeInBits(Expr->getType());
    auto *DivisorDom = isl_pw_aff_domain(isl_pw_aff_copy(DivisorPWAC.first));
    auto *WidthExpPWA = getWidthExpValOnDomain(Width, DivisorDom);
    DivisorPWAC.first = isl_pw_aff_add(DivisorPWAC.first, WidthExpPWA);
  }

  // TODO: One can represent the dividend as piece-wise function to be more
  //       precise but therefor a heuristic is needed.

  // Assume a non-negative dividend.
  takeNonNegativeAssumption(DividendPWAC);

  combine(DividendPWAC, DivisorPWAC, isl_pw_aff_div);
  DividendPWAC.first = isl_pw_aff_floor(DividendPWAC.first);

  return DividendPWAC;
}

__isl_give PWACtx SCEVAffinator::visitSDivInstruction(Instruction *SDiv) {
  assert(SDiv->getOpcode() == Instruction::SDiv && "Assumed SDiv instruction!");
  auto *SE = S->getSE();

  auto *Divisor = SDiv->getOperand(1);
  auto *DivisorSCEV = SE->getSCEV(Divisor);
  auto DivisorPWAC = visit(DivisorSCEV);
  assert(isa<ConstantInt>(Divisor) &&
         "SDiv is no parameter but has a non-constant RHS.");

  auto *Dividend = SDiv->getOperand(0);
  auto *DividendSCEV = SE->getSCEV(Dividend);
  auto DividendPWAC = visit(DividendSCEV);
  combine(DividendPWAC, DivisorPWAC, isl_pw_aff_tdiv_q);
  return DividendPWAC;
}

__isl_give PWACtx SCEVAffinator::visitSRemInstruction(Instruction *SRem) {
  assert(SRem->getOpcode() == Instruction::SRem && "Assumed SRem instruction!");
  auto *SE = S->getSE();

  auto *Divisor = dyn_cast<ConstantInt>(SRem->getOperand(1));
  assert(Divisor && "SRem is no parameter but has a non-constant RHS.");
  auto *DivisorVal = isl_valFromAPInt(Ctx, Divisor->getValue(),
                                      /* isSigned */ true);

  auto *Dividend = SRem->getOperand(0);
  auto *DividendSCEV = SE->getSCEV(Dividend);
  auto DividendPWAC = visit(DividendSCEV);

  DividendPWAC.first =
      isl_pw_aff_mod_val(DividendPWAC.first, isl_val_abs(DivisorVal));
  return DividendPWAC;
}

__isl_give PWACtx SCEVAffinator::visitUnknown(const SCEVUnknown *Expr) {
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
