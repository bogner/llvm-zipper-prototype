// RUN: %check_clang_tidy %s performance-unnecessary-value-param %t -- -fix-errors -- --std=c++11

// CHECK-FIXES: #include <utility>

struct ExpensiveToCopyType {
  const ExpensiveToCopyType & constReference() const {
    return *this;
  }
  void nonConstMethod();
  virtual ~ExpensiveToCopyType();
};

void mutate(ExpensiveToCopyType &);
void mutate(ExpensiveToCopyType *);
void useAsConstReference(const ExpensiveToCopyType &);
void useByValue(ExpensiveToCopyType);

// This class simulates std::pair<>. It is trivially copy constructible
// and trivially destructible, but not trivially copy assignable.
class SomewhatTrivial {
 public:
  SomewhatTrivial();
  SomewhatTrivial(const SomewhatTrivial&) = default;
  ~SomewhatTrivial() = default;
  SomewhatTrivial& operator=(const SomewhatTrivial&);
};

struct MoveOnlyType {
  MoveOnlyType(const MoveOnlyType &) = delete;
  MoveOnlyType(MoveOnlyType &&) = default;
  ~MoveOnlyType();
  void constMethod() const;
};

struct ExpensiveMovableType {
  ExpensiveMovableType();
  ExpensiveMovableType(ExpensiveMovableType &&);
  ExpensiveMovableType(const ExpensiveMovableType &) = default;
  ExpensiveMovableType &operator=(const ExpensiveMovableType &) = default;
  ExpensiveMovableType &operator=(ExpensiveMovableType &&);
  ~ExpensiveMovableType();
};

void positiveExpensiveConstValue(const ExpensiveToCopyType Obj);
// CHECK-FIXES: void positiveExpensiveConstValue(const ExpensiveToCopyType& Obj);
void positiveExpensiveConstValue(const ExpensiveToCopyType Obj) {
  // CHECK-MESSAGES: [[@LINE-1]]:60: warning: the const qualified parameter 'Obj' is copied for each invocation; consider making it a reference [performance-unnecessary-value-param]
  // CHECK-FIXES: void positiveExpensiveConstValue(const ExpensiveToCopyType& Obj) {
}

void positiveExpensiveValue(ExpensiveToCopyType Obj);
// CHECK-FIXES: void positiveExpensiveValue(const ExpensiveToCopyType& Obj);
void positiveExpensiveValue(ExpensiveToCopyType Obj) {
  // CHECK-MESSAGES: [[@LINE-1]]:49: warning: the parameter 'Obj' is copied for each invocation but only used as a const reference; consider making it a const reference [performance-unnecessary-value-param]
  // CHECK-FIXES: void positiveExpensiveValue(const ExpensiveToCopyType& Obj) {
  Obj.constReference();
  useAsConstReference(Obj);
  auto Copy = Obj;
  useByValue(Obj);
}

void positiveWithComment(const ExpensiveToCopyType /* important */ S);
// CHECK-FIXES: void positiveWithComment(const ExpensiveToCopyType& /* important */ S);
void positiveWithComment(const ExpensiveToCopyType /* important */ S) {
  // CHECK-MESSAGES: [[@LINE-1]]:68: warning: the const qualified
  // CHECK-FIXES: void positiveWithComment(const ExpensiveToCopyType& /* important */ S) {
}

void positiveUnnamedParam(const ExpensiveToCopyType) {
  // CHECK-MESSAGES: [[@LINE-1]]:52: warning: the const qualified parameter #1
  // CHECK-FIXES: void positiveUnnamedParam(const ExpensiveToCopyType&) {
}

void positiveAndNegative(const ExpensiveToCopyType ConstCopy, const ExpensiveToCopyType& ConstRef, ExpensiveToCopyType Copy);
// CHECK-FIXES: void positiveAndNegative(const ExpensiveToCopyType& ConstCopy, const ExpensiveToCopyType& ConstRef, const ExpensiveToCopyType& Copy);
void positiveAndNegative(const ExpensiveToCopyType ConstCopy, const ExpensiveToCopyType& ConstRef, ExpensiveToCopyType Copy) {
  // CHECK-MESSAGES: [[@LINE-1]]:52: warning: the const qualified parameter 'ConstCopy'
  // CHECK-MESSAGES: [[@LINE-2]]:120: warning: the parameter 'Copy'
  // CHECK-FIXES: void positiveAndNegative(const ExpensiveToCopyType& ConstCopy, const ExpensiveToCopyType& ConstRef, const ExpensiveToCopyType& Copy) {
}

struct PositiveConstValueConstructor {
  PositiveConstValueConstructor(const ExpensiveToCopyType ConstCopy) {}
  // CHECK-MESSAGES: [[@LINE-1]]:59: warning: the const qualified parameter 'ConstCopy'
};

template <typename T> void templateWithNonTemplatizedParameter(const ExpensiveToCopyType S, T V) {
  // CHECK-MESSAGES: [[@LINE-1]]:90: warning: the const qualified parameter 'S'
  // CHECK-FIXES: template <typename T> void templateWithNonTemplatizedParameter(const ExpensiveToCopyType& S, T V) {
}

void instantiated() {
  templateWithNonTemplatizedParameter(ExpensiveToCopyType(), ExpensiveToCopyType());
  templateWithNonTemplatizedParameter(ExpensiveToCopyType(), 5);
}

template <typename T> void negativeTemplateType(const T V) {
}

void negativeArray(const ExpensiveToCopyType[]) {
}

void negativePointer(ExpensiveToCopyType* Obj) {
}

void negativeConstPointer(const ExpensiveToCopyType* Obj) {
}

void negativeConstReference(const ExpensiveToCopyType& Obj) {
}

void negativeReference(ExpensiveToCopyType& Obj) {
}

void negativeUniversalReference(ExpensiveToCopyType&& Obj) {
}

void negativeSomewhatTrivialConstValue(const SomewhatTrivial Somewhat) {
}

void negativeSomewhatTrivialValue(SomewhatTrivial Somewhat) {
}

void negativeConstBuiltIn(const int I) {
}

void negativeValueBuiltIn(int I) {
}

void negativeValueIsMutatedByReference(ExpensiveToCopyType Obj) {
  mutate(Obj);
}

void negativeValueIsMutatatedByPointer(ExpensiveToCopyType Obj) {
  mutate(&Obj);
}

void negativeValueIsReassigned(ExpensiveToCopyType Obj) {
  Obj = ExpensiveToCopyType();
}

void negativeValueNonConstMethodIsCalled(ExpensiveToCopyType Obj) {
  Obj.nonConstMethod();
}

struct NegativeValueCopyConstructor {
  NegativeValueCopyConstructor(ExpensiveToCopyType Copy) {}
};

template <typename T>
struct Container {
  typedef const T & const_reference;
};

void NegativeTypedefParam(const Container<ExpensiveToCopyType>::const_reference Param) {
}

#define UNNECESSARY_VALUE_PARAM_IN_MACRO_BODY()         \
  void inMacro(const ExpensiveToCopyType T) {           \
  }                                                     \
// Ensure fix is not applied.
// CHECK-FIXES: void inMacro(const ExpensiveToCopyType T) {

UNNECESSARY_VALUE_PARAM_IN_MACRO_BODY()
// CHECK-MESSAGES: [[@LINE-1]]:1: warning: the const qualified parameter 'T'

#define UNNECESSARY_VALUE_PARAM_IN_MACRO_ARGUMENT(ARGUMENT)     \
  ARGUMENT

UNNECESSARY_VALUE_PARAM_IN_MACRO_ARGUMENT(void inMacroArgument(const ExpensiveToCopyType InMacroArg) {})
// CHECK-MESSAGES: [[@LINE-1]]:90: warning: the const qualified parameter 'InMacroArg'
// CHECK-FIXES: void inMacroArgument(const ExpensiveToCopyType InMacroArg) {}

struct VirtualMethod {
  virtual ~VirtualMethod() {}
  virtual void handle(ExpensiveToCopyType T) const = 0;
};

struct NegativeOverriddenMethod : public VirtualMethod {
  void handle(ExpensiveToCopyType Overridden) const {
    // CHECK-FIXES: handle(ExpensiveToCopyType Overridden) const {
  }
};

struct VirtualMethodWarningOnly {
  virtual void methodWithExpensiveValueParam(ExpensiveToCopyType T) {}
  // CHECK-MESSAGES: [[@LINE-1]]:66: warning: the parameter 'T' is copied
  // CHECK-FIXES: virtual void methodWithExpensiveValueParam(ExpensiveToCopyType T) {}
  virtual ~VirtualMethodWarningOnly() {}
};

struct PositiveNonVirualMethod {
  void method(const ExpensiveToCopyType T) {}
  // CHECK-MESSAGES: [[@LINE-1]]:41: warning: the const qualified parameter 'T' is copied
  // CHECK-FIXES: void method(const ExpensiveToCopyType& T) {}
};

struct NegativeDeletedMethod {
  ~NegativeDeletedMethod() {}
  NegativeDeletedMethod& operator=(NegativeDeletedMethod N) = delete;
  // CHECK-FIXES: NegativeDeletedMethod& operator=(NegativeDeletedMethod N) = delete;
};

void NegativeMoveOnlyTypePassedByValue(MoveOnlyType M) {
  M.constMethod();
}

void PositiveMoveOnCopyConstruction(ExpensiveMovableType E) {
  auto F = E;
  // CHECK-MESSAGES: [[@LINE-1]]:12: warning: parameter 'E' is passed by value and only copied once; consider moving it to avoid unnecessary copies [performance-unnecessary-value-param]
  // CHECK-FIXES: auto F = std::move(E);
}

void PositiveConstRefNotMoveSinceReferencedMultipleTimes(ExpensiveMovableType E) {
  // CHECK-MESSAGES: [[@LINE-1]]:79: warning: the parameter 'E' is copied
  // CHECK-FIXES: void PositiveConstRefNotMoveSinceReferencedMultipleTimes(const ExpensiveMovableType& E) {
  auto F = E;
  auto G = E;
}

void PositiveMoveOnCopyAssignment(ExpensiveMovableType E) {
  ExpensiveMovableType F;
  F = E;
  // CHECK-MESSAGES: [[@LINE-1]]:7: warning: parameter 'E' is passed by value
  // CHECK-FIXES: F = std::move(E);
}

void PositiveConstRefNotMoveConstructible(ExpensiveToCopyType T) {
  // CHECK-MESSAGES: [[@LINE-1]]:63: warning: the parameter 'T' is copied
  // CHECK-FIXES: void PositiveConstRefNotMoveConstructible(const ExpensiveToCopyType& T) {
  auto U = T;
}

void PositiveConstRefNotMoveAssignable(ExpensiveToCopyType A) {
  // CHECK-MESSAGES: [[@LINE-1]]:60: warning: the parameter 'A' is copied
  // CHECK-FIXES: void PositiveConstRefNotMoveAssignable(const ExpensiveToCopyType& A) {
  ExpensiveToCopyType B;
  B = A;
}

// Ensure that incomplete types result in an error from the frontend and not a
// clang-tidy diagnostic about IncompleteType being expensive to copy.
struct IncompleteType;
void NegativeForIncompleteType(IncompleteType I) {
  // CHECK-MESSAGES: [[@LINE-1]]:47: error: variable has incomplete type 'IncompleteType' [clang-diagnostic-error]
}

// Case where parameter in declaration is already const-qualified but not in
// implementation. Make sure a second 'const' is not added to the declaration.
void PositiveConstDeclaration(const ExpensiveToCopyType A);
// CHECK-FIXES: void PositiveConstDeclaration(const ExpensiveToCopyType& A);
void PositiveConstDeclaration(ExpensiveToCopyType A) {
  // CHECK-MESSAGES: [[@LINE-1]]:51: warning: the parameter 'A' is copied
  // CHECK-FIXES: void PositiveConstDeclaration(const ExpensiveToCopyType& A) {
}

void PositiveNonConstDeclaration(ExpensiveToCopyType A);
// CHECK-FIXES: void PositiveNonConstDeclaration(const ExpensiveToCopyType& A);
void PositiveNonConstDeclaration(const ExpensiveToCopyType A) {
  // CHECK-MESSAGES: [[@LINE-1]]:60: warning: the const qualified parameter 'A'
  // CHECK-FIXES: void PositiveNonConstDeclaration(const ExpensiveToCopyType& A) {
}
