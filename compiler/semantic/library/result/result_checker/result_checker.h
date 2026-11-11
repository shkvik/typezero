#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/result/result_checker/result_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FieldFacts;
class CallChecker;
class ExprChecker;
class ResultTypes;
class AsyncChecker;
class TypeResolver;
class TypeRelations;
class ClosureChecker;
class ArgumentChecker;

class ResultChecker {
public:
  using ResultValueGetsUsed = Result<const Expr *, ResultCheckerUsedValueError>;

  ResultChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkResultCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkResultStatic(CallExpr *call, MemberExpr *member);
  Expr *checkResultMember(MemberExpr *member);
  Expr *checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected);
  ResultValueGetsUsed usedValue(const Expr *expr, bool callback = false, llvm::SMLoc at = {});
  void addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  Expr *markCallFailed(CallExpr *call);
  void checkArgsLoosely(CallExpr *call);

private:
  using ResultStaticIsKnown = Result<llvm::StringRef, ResultCheckerStaticMethodError>;
  using ResultOkErrTypeArgs = Result<const NameExpr *, ResultCheckerOkErrTypeArgsError>;

  static ResultOkErrTypeArgs okErrTypeArgs(const NameExpr *name);
  static ResultStaticIsKnown staticMethod(const NameExpr *name, llvm::StringRef method);

  struct CombineCall {
    CallExpr *call = nullptr;
    std::string what;
    bool all = false;
  };

  bool combineArityFits(CallExpr *call, const MemberExpr *member);
  static bool isPlainList(const ArrayLitExpr *lit);
  bool holdsValues(const CombineCall &combine, const Type *result, const Expr *at);
  static const Type *errorsOf(const CombineCall &combine, const Type *error);
  Expr *checkCombineList(const CombineCall &combine, ArrayLitExpr *lit);
  static const Type *sameValues(const Type *a, const Type *b);
  static const Type *commonResult(llvm::ArrayRef<const Type *> results);
  Expr *checkSameCombine(const CombineCall &combine, ArrayLitExpr *lit, const Type *common);
  Expr *checkTupleCombine(const CombineCall &combine, ArrayLitExpr *lit,
                          const std::vector<const Type *> &results);
  Expr *checkCombineArray(const CombineCall &combine);
  static const Type *halfParam(const Type *half);
  static bool givesType(const Type *half);
  static std::string receiverText(const Expr *expr);
  static const Expr *unconverted(const Expr *expr);

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ResultTypes *resultTypes = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
