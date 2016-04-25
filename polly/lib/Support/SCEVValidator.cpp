
#include "polly/Support/SCEVValidator.h"
#include "polly/ScopInfo.h"
#include "llvm/Analysis/RegionInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace polly;

#define DEBUG_TYPE "polly-scev-validator"

namespace SCEVType {
/// @brief The type of a SCEV
///
/// To check for the validity of a SCEV we assign to each SCEV a type. The
/// possible types are INT, PARAM, IV and INVALID. The order of the types is
/// important. The subexpressions of SCEV with a type X can only have a type
/// that is smaller or equal than X.
enum TYPE {
  // An integer value.
  INT,

  // An expression that is constant during the execution of the Scop,
  // but that may depend on parameters unknown at compile time.
  PARAM,

  // An expression that may change during the execution of the SCoP.
  IV,

  // An invalid expression.
  INVALID
};
}

/// @brief The result the validator returns for a SCEV expression.
class ValidatorResult {
  /// @brief The type of the expression
  SCEVType::TYPE Type;

  /// @brief The set of Parameters in the expression.
  ParameterSetTy Parameters;

public:
  /// @brief The copy constructor
  ValidatorResult(const ValidatorResult &Source) {
    Type = Source.Type;
    Parameters = Source.Parameters;
  }

  /// @brief Construct a result with a certain type and no parameters.
  ValidatorResult(SCEVType::TYPE Type) : Type(Type) {
    assert(Type != SCEVType::PARAM && "Did you forget to pass the parameter");
  }

  /// @brief Construct a result with a certain type and a single parameter.
  ValidatorResult(SCEVType::TYPE Type, const SCEV *Expr) : Type(Type) {
    Parameters.insert(Expr);
  }

  /// @brief Get the type of the ValidatorResult.
  SCEVType::TYPE getType() { return Type; }

  /// @brief Is the analyzed SCEV constant during the execution of the SCoP.
  bool isConstant() { return Type == SCEVType::INT || Type == SCEVType::PARAM; }

  /// @brief Is the analyzed SCEV valid.
  bool isValid() { return Type != SCEVType::INVALID; }

  /// @brief Is the analyzed SCEV of Type IV.
  bool isIV() { return Type == SCEVType::IV; }

  /// @brief Is the analyzed SCEV of Type INT.
  bool isINT() { return Type == SCEVType::INT; }

  /// @brief Is the analyzed SCEV of Type PARAM.
  bool isPARAM() { return Type == SCEVType::PARAM; }

  /// @brief Get the parameters of this validator result.
  const ParameterSetTy &getParameters() { return Parameters; }

  /// @brief Add the parameters of Source to this result.
  void addParamsFrom(const ValidatorResult &Source) {
    Parameters.insert(Source.Parameters.begin(), Source.Parameters.end());
  }

  /// @brief Merge a result.
  ///
  /// This means to merge the parameters and to set the Type to the most
  /// specific Type that matches both.
  void merge(const ValidatorResult &ToMerge) {
    Type = std::max(Type, ToMerge.Type);
    addParamsFrom(ToMerge);
  }

  void print(raw_ostream &OS) {
    switch (Type) {
    case SCEVType::INT:
      OS << "SCEVType::INT";
      break;
    case SCEVType::PARAM:
      OS << "SCEVType::PARAM";
      break;
    case SCEVType::IV:
      OS << "SCEVType::IV";
      break;
    case SCEVType::INVALID:
      OS << "SCEVType::INVALID";
      break;
    }
  }
};

raw_ostream &operator<<(raw_ostream &OS, class ValidatorResult &VR) {
  VR.print(OS);
  return OS;
}

/// Check if a SCEV is valid in a SCoP.
struct SCEVValidator
    : public SCEVVisitor<SCEVValidator, class ValidatorResult> {
private:
  const Region *R;
  Loop *Scope;
  ScalarEvolution &SE;
  InvariantLoadsSetTy *ILS;

public:
  SCEVValidator(const Region *R, Loop *Scope, ScalarEvolution &SE,
                InvariantLoadsSetTy *ILS)
      : R(R), Scope(Scope), SE(SE), ILS(ILS) {}

  class ValidatorResult visitConstant(const SCEVConstant *Constant) {
    return ValidatorResult(SCEVType::INT);
  }

  class ValidatorResult visitTruncateExpr(const SCEVTruncateExpr *Expr) {
    ValidatorResult Op = visit(Expr->getOperand());

    switch (Op.getType()) {
    case SCEVType::INT:
    case SCEVType::PARAM:
      // We currently do not represent a truncate expression as an affine
      // expression. If it is constant during Scop execution, we treat it as a
      // parameter.
      return ValidatorResult(SCEVType::PARAM, Expr);
    case SCEVType::IV:
      DEBUG(dbgs() << "INVALID: Truncation of SCEVType::IV expression");
      return ValidatorResult(SCEVType::INVALID);
    case SCEVType::INVALID:
      return Op;
    }

    llvm_unreachable("Unknown SCEVType");
  }

  class ValidatorResult visitZeroExtendExpr(const SCEVZeroExtendExpr *Expr) {
    ValidatorResult Op = visit(Expr->getOperand());

    switch (Op.getType()) {
    case SCEVType::INT:
    case SCEVType::PARAM:
      // We currently do not represent a truncate expression as an affine
      // expression. If it is constant during Scop execution, we treat it as a
      // parameter.
      return ValidatorResult(SCEVType::PARAM, Expr);
    case SCEVType::IV:
      DEBUG(dbgs() << "INVALID: ZeroExtend of SCEVType::IV expression");
      return ValidatorResult(SCEVType::INVALID);
    case SCEVType::INVALID:
      return Op;
    }

    llvm_unreachable("Unknown SCEVType");
  }

  class ValidatorResult visitSignExtendExpr(const SCEVSignExtendExpr *Expr) {
    // We currently allow only signed SCEV expressions. In the case of a
    // signed value, a sign extend is a noop.
    //
    // TODO: Reconsider this when we add support for unsigned values.
    return visit(Expr->getOperand());
  }

  class ValidatorResult visitAddExpr(const SCEVAddExpr *Expr) {
    ValidatorResult Return(SCEVType::INT);

    for (int i = 0, e = Expr->getNumOperands(); i < e; ++i) {
      ValidatorResult Op = visit(Expr->getOperand(i));
      Return.merge(Op);

      // Early exit.
      if (!Return.isValid())
        break;
    }

    // TODO: Check for NSW and NUW.
    return Return;
  }

  class ValidatorResult visitMulExpr(const SCEVMulExpr *Expr) {
    ValidatorResult Return(SCEVType::INT);

    bool HasMultipleParams = false;

    for (int i = 0, e = Expr->getNumOperands(); i < e; ++i) {
      ValidatorResult Op = visit(Expr->getOperand(i));

      if (Op.isINT())
        continue;

      if (Op.isPARAM() && Return.isPARAM()) {
        HasMultipleParams = true;
        continue;
      }

      if ((Op.isIV() || Op.isPARAM()) && !Return.isINT()) {
        DEBUG(dbgs() << "INVALID: More than one non-int operand in MulExpr\n"
                     << "\tExpr: " << *Expr << "\n"
                     << "\tPrevious expression type: " << Return << "\n"
                     << "\tNext operand (" << Op
                     << "): " << *Expr->getOperand(i) << "\n");

        return ValidatorResult(SCEVType::INVALID);
      }

      Return.merge(Op);
    }

    if (HasMultipleParams && Return.isValid())
      return ValidatorResult(SCEVType::PARAM, Expr);

    // TODO: Check for NSW and NUW.
    return Return;
  }

  class ValidatorResult visitUDivExpr(const SCEVUDivExpr *Expr) {
    ValidatorResult LHS = visit(Expr->getLHS());
    ValidatorResult RHS = visit(Expr->getRHS());

    // We currently do not represent an unsigned division as an affine
    // expression. If the division is constant during Scop execution we treat it
    // as a parameter, otherwise we bail out.
    if (LHS.isConstant() && RHS.isConstant())
      return ValidatorResult(SCEVType::PARAM, Expr);

    DEBUG(dbgs() << "INVALID: unsigned division of non-constant expressions");
    return ValidatorResult(SCEVType::INVALID);
  }

  class ValidatorResult visitAddRecExpr(const SCEVAddRecExpr *Expr) {
    if (!Expr->isAffine()) {
      DEBUG(dbgs() << "INVALID: AddRec is not affine");
      return ValidatorResult(SCEVType::INVALID);
    }

    ValidatorResult Start = visit(Expr->getStart());
    ValidatorResult Recurrence = visit(Expr->getStepRecurrence(SE));

    if (!Start.isValid())
      return Start;

    if (!Recurrence.isValid())
      return Recurrence;

    auto *L = Expr->getLoop();
    if (R->contains(L) && (!Scope || !L->contains(Scope))) {
      DEBUG(dbgs() << "INVALID: AddRec out of a loop whose exit value is not "
                      "synthesizable");
      return ValidatorResult(SCEVType::INVALID);
    }

    if (R->contains(L)) {
      if (Recurrence.isINT()) {
        ValidatorResult Result(SCEVType::IV);
        Result.addParamsFrom(Start);
        return Result;
      }

      DEBUG(dbgs() << "INVALID: AddRec within scop has non-int"
                      "recurrence part");
      return ValidatorResult(SCEVType::INVALID);
    }

    assert(Start.isConstant() && Recurrence.isConstant() &&
           "Expected 'Start' and 'Recurrence' to be constant");

    // Directly generate ValidatorResult for Expr if 'start' is zero.
    if (Expr->getStart()->isZero())
      return ValidatorResult(SCEVType::PARAM, Expr);

    // Translate AddRecExpr from '{start, +, inc}' into 'start + {0, +, inc}'
    // if 'start' is not zero.
    const SCEV *ZeroStartExpr = SE.getAddRecExpr(
        SE.getConstant(Expr->getStart()->getType(), 0),
        Expr->getStepRecurrence(SE), Expr->getLoop(), Expr->getNoWrapFlags());

    ValidatorResult ZeroStartResult =
        ValidatorResult(SCEVType::PARAM, ZeroStartExpr);
    ZeroStartResult.addParamsFrom(Start);

    return ZeroStartResult;
  }

  class ValidatorResult visitSMaxExpr(const SCEVSMaxExpr *Expr) {
    ValidatorResult Return(SCEVType::INT);

    for (int i = 0, e = Expr->getNumOperands(); i < e; ++i) {
      ValidatorResult Op = visit(Expr->getOperand(i));

      if (!Op.isValid())
        return Op;

      Return.merge(Op);
    }

    return Return;
  }

  class ValidatorResult visitUMaxExpr(const SCEVUMaxExpr *Expr) {
    // We do not support unsigned operations. If 'Expr' is constant during Scop
    // execution we treat this as a parameter, otherwise we bail out.
    for (int i = 0, e = Expr->getNumOperands(); i < e; ++i) {
      ValidatorResult Op = visit(Expr->getOperand(i));

      if (!Op.isConstant()) {
        DEBUG(dbgs() << "INVALID: UMaxExpr has a non-constant operand");
        return ValidatorResult(SCEVType::INVALID);
      }
    }

    return ValidatorResult(SCEVType::PARAM, Expr);
  }

  ValidatorResult visitGenericInst(Instruction *I, const SCEV *S) {
    if (R->contains(I)) {
      DEBUG(dbgs() << "INVALID: UnknownExpr references an instruction "
                      "within the region\n");
      return ValidatorResult(SCEVType::INVALID);
    }

    return ValidatorResult(SCEVType::PARAM, S);
  }

  ValidatorResult visitLoadInstruction(Instruction *I, const SCEV *S) {
    if (R->contains(I) && ILS) {
      ILS->insert(cast<LoadInst>(I));
      return ValidatorResult(SCEVType::PARAM, S);
    }

    return visitGenericInst(I, S);
  }

  ValidatorResult visitSDivInstruction(Instruction *SDiv, const SCEV *S) {
    assert(SDiv->getOpcode() == Instruction::SDiv &&
           "Assumed SDiv instruction!");

    auto *Divisor = SDiv->getOperand(1);
    auto *CI = dyn_cast<ConstantInt>(Divisor);
    if (!CI)
      return visitGenericInst(SDiv, S);

    auto *Dividend = SDiv->getOperand(0);
    auto *DividendSCEV = SE.getSCEV(Dividend);
    return visit(DividendSCEV);
  }

  ValidatorResult visitSRemInstruction(Instruction *SRem, const SCEV *S) {
    assert(SRem->getOpcode() == Instruction::SRem &&
           "Assumed SRem instruction!");

    auto *Divisor = SRem->getOperand(1);
    auto *CI = dyn_cast<ConstantInt>(Divisor);
    if (!CI)
      return visitGenericInst(SRem, S);

    auto *Dividend = SRem->getOperand(0);
    auto *DividendSCEV = SE.getSCEV(Dividend);
    return visit(DividendSCEV);
  }

  ValidatorResult visitUnknown(const SCEVUnknown *Expr) {
    Value *V = Expr->getValue();

    if (!Expr->getType()->isIntegerTy() && !Expr->getType()->isPointerTy()) {
      DEBUG(dbgs() << "INVALID: UnknownExpr is not an integer or pointer");
      return ValidatorResult(SCEVType::INVALID);
    }

    if (isa<UndefValue>(V)) {
      DEBUG(dbgs() << "INVALID: UnknownExpr references an undef value");
      return ValidatorResult(SCEVType::INVALID);
    }

    if (Instruction *I = dyn_cast<Instruction>(Expr->getValue())) {
      switch (I->getOpcode()) {
      case Instruction::Load:
        return visitLoadInstruction(I, Expr);
      case Instruction::SDiv:
        return visitSDivInstruction(I, Expr);
      case Instruction::SRem:
        return visitSRemInstruction(I, Expr);
      default:
        return visitGenericInst(I, Expr);
      }
    }

    return ValidatorResult(SCEVType::PARAM, Expr);
  }
};

/// @brief Check whether a SCEV refers to an SSA name defined inside a region.
class SCEVInRegionDependences {
  const Region *R;
  Loop *Scope;
  bool AllowLoops;
  bool HasInRegionDeps = false;

public:
  SCEVInRegionDependences(const Region *R, Loop *Scope, bool AllowLoops)
      : R(R), Scope(Scope), AllowLoops(AllowLoops) {}

  bool follow(const SCEV *S) {
    if (auto Unknown = dyn_cast<SCEVUnknown>(S)) {
      Instruction *Inst = dyn_cast<Instruction>(Unknown->getValue());

      // Return true when Inst is defined inside the region R.
      if (Inst && R->contains(Inst)) {
        HasInRegionDeps = true;
        return false;
      }
    } else if (auto AddRec = dyn_cast<SCEVAddRecExpr>(S)) {
      if (!AllowLoops) {
        if (!Scope) {
          HasInRegionDeps = true;
          return false;
        }
        auto *L = AddRec->getLoop();
        if (R->contains(L) && !L->contains(Scope)) {
          HasInRegionDeps = true;
          return false;
        }
      }
    }
    return true;
  }
  bool isDone() { return false; }
  bool hasDependences() { return HasInRegionDeps; }
};

namespace polly {
/// Find all loops referenced in SCEVAddRecExprs.
class SCEVFindLoops {
  SetVector<const Loop *> &Loops;

public:
  SCEVFindLoops(SetVector<const Loop *> &Loops) : Loops(Loops) {}

  bool follow(const SCEV *S) {
    if (const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(S))
      Loops.insert(AddRec->getLoop());
    return true;
  }
  bool isDone() { return false; }
};

void findLoops(const SCEV *Expr, SetVector<const Loop *> &Loops) {
  SCEVFindLoops FindLoops(Loops);
  SCEVTraversal<SCEVFindLoops> ST(FindLoops);
  ST.visitAll(Expr);
}

/// Find all values referenced in SCEVUnknowns.
class SCEVFindValues {
  ScalarEvolution &SE;
  SetVector<Value *> &Values;

public:
  SCEVFindValues(ScalarEvolution &SE, SetVector<Value *> &Values)
      : SE(SE), Values(Values) {}

  bool follow(const SCEV *S) {
    const SCEVUnknown *Unknown = dyn_cast<SCEVUnknown>(S);
    if (!Unknown)
      return true;

    Values.insert(Unknown->getValue());
    Instruction *Inst = dyn_cast<Instruction>(Unknown->getValue());
    if (!Inst || (Inst->getOpcode() != Instruction::SRem &&
                  Inst->getOpcode() != Instruction::SDiv))
      return false;

    auto *Dividend = SE.getSCEV(Inst->getOperand(1));
    if (!isa<SCEVConstant>(Dividend))
      return false;

    auto *Divisor = SE.getSCEV(Inst->getOperand(0));
    SCEVFindValues FindValues(SE, Values);
    SCEVTraversal<SCEVFindValues> ST(FindValues);
    ST.visitAll(Dividend);
    ST.visitAll(Divisor);

    return false;
  }
  bool isDone() { return false; }
};

void findValues(const SCEV *Expr, ScalarEvolution &SE,
                SetVector<Value *> &Values) {
  SCEVFindValues FindValues(SE, Values);
  SCEVTraversal<SCEVFindValues> ST(FindValues);
  ST.visitAll(Expr);
}

bool hasScalarDepsInsideRegion(const SCEV *Expr, const Region *R,
                               llvm::Loop *Scope, bool AllowLoops) {
  SCEVInRegionDependences InRegionDeps(R, Scope, AllowLoops);
  SCEVTraversal<SCEVInRegionDependences> ST(InRegionDeps);
  ST.visitAll(Expr);
  return InRegionDeps.hasDependences();
}

bool isAffineExpr(const Region *R, llvm::Loop *Scope, const SCEV *Expr,
                  ScalarEvolution &SE, InvariantLoadsSetTy *ILS) {
  if (isa<SCEVCouldNotCompute>(Expr))
    return false;

  SCEVValidator Validator(R, Scope, SE, ILS);
  DEBUG({
    dbgs() << "\n";
    dbgs() << "Expr: " << *Expr << "\n";
    dbgs() << "Region: " << R->getNameStr() << "\n";
    dbgs() << " -> ";
  });

  ValidatorResult Result = Validator.visit(Expr);

  DEBUG({
    if (Result.isValid())
      dbgs() << "VALID\n";
    dbgs() << "\n";
  });

  return Result.isValid();
}

static bool isAffineParamExpr(Value *V, const Region *R, Loop *Scope,
                              ScalarEvolution &SE, ParameterSetTy &Params) {
  auto *E = SE.getSCEV(V);
  if (isa<SCEVCouldNotCompute>(E))
    return false;

  SCEVValidator Validator(R, Scope, SE, nullptr);
  ValidatorResult Result = Validator.visit(E);
  if (!Result.isConstant())
    return false;

  auto ResultParams = Result.getParameters();
  Params.insert(ResultParams.begin(), ResultParams.end());

  return true;
}

bool isAffineParamConstraint(Value *V, const Region *R, llvm::Loop *Scope,
                             ScalarEvolution &SE, ParameterSetTy &Params,
                             bool OrExpr) {
  if (auto *ICmp = dyn_cast<ICmpInst>(V)) {
    return isAffineParamConstraint(ICmp->getOperand(0), R, Scope, SE, Params,
                                   true) &&
           isAffineParamConstraint(ICmp->getOperand(1), R, Scope, SE, Params,
                                   true);
  } else if (auto *BinOp = dyn_cast<BinaryOperator>(V)) {
    auto Opcode = BinOp->getOpcode();
    if (Opcode == Instruction::And || Opcode == Instruction::Or)
      return isAffineParamConstraint(BinOp->getOperand(0), R, Scope, SE, Params,
                                     false) &&
             isAffineParamConstraint(BinOp->getOperand(1), R, Scope, SE, Params,
                                     false);
    /* Fall through */
  }

  if (!OrExpr)
    return false;

  return isAffineParamExpr(V, R, Scope, SE, Params);
}

ParameterSetTy getParamsInAffineExpr(const Region *R, Loop *Scope,
                                     const SCEV *Expr, ScalarEvolution &SE) {
  if (isa<SCEVCouldNotCompute>(Expr))
    return ParameterSetTy();

  InvariantLoadsSetTy ILS;
  SCEVValidator Validator(R, Scope, SE, &ILS);
  ValidatorResult Result = Validator.visit(Expr);
  assert(Result.isValid() && "Requested parameters for an invalid SCEV!");

  return Result.getParameters();
}

std::pair<const SCEVConstant *, const SCEV *>
extractConstantFactor(const SCEV *S, ScalarEvolution &SE) {

  auto *LeftOver = SE.getConstant(S->getType(), 1);
  auto *ConstPart = cast<SCEVConstant>(SE.getConstant(S->getType(), 1));

  if (auto *Constant = dyn_cast<SCEVConstant>(S))
    return std::make_pair(Constant, LeftOver);

  auto *AddRec = dyn_cast<SCEVAddRecExpr>(S);
  if (AddRec) {
    auto *StartExpr = AddRec->getStart();
    if (StartExpr->isZero()) {
      auto StepPair = extractConstantFactor(AddRec->getStepRecurrence(SE), SE);
      auto *LeftOverAddRec =
          SE.getAddRecExpr(StartExpr, StepPair.second, AddRec->getLoop(),
                           AddRec->getNoWrapFlags());
      return std::make_pair(StepPair.first, LeftOverAddRec);
    }
    return std::make_pair(ConstPart, S);
  }

  auto *Mul = dyn_cast<SCEVMulExpr>(S);
  if (!Mul)
    return std::make_pair(ConstPart, S);

  for (auto *Op : Mul->operands())
    if (isa<SCEVConstant>(Op))
      ConstPart = cast<SCEVConstant>(SE.getMulExpr(ConstPart, Op));
    else
      LeftOver = SE.getMulExpr(LeftOver, Op);

  return std::make_pair(ConstPart, LeftOver);
}
}
