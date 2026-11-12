#pragma once

#include "compiler/program/library/result/result_static_table/result_static_table.h"
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

  Expr *markCallFailed(CallExpr *call);
  Expr *checkResultMember(MemberExpr *member);
  ResultValueGetsUsed usedValue(const Expr *expr);
  ResultValueGetsUsed usedCallbackValue(const Expr *expr);
  Expr *checkResultStatic(CallExpr *call, MemberExpr *member);
  ResultValueGetsUsed usedValue(const Expr *expr, llvm::SMLoc at);
  Expr *checkResultCall(CallExpr *call, NameExpr *name, const Type *expected);
  void addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  enum class Half {
    OK,
    ERR,
  };

  enum class Discarded {
    BY_STATEMENT,
    BY_CALLBACK,
  };

  struct CombineCall {
    CallExpr *call;
    std::string what;
    bool isAll;
  };

  using ResultStaticIsKnown = Result<ResultStatic, ResultCheckerStaticMethodError>;
  using ResultOkErrTypeArgs = Result<const Type *, ResultCheckerOkErrTypeArgsError>;

  ResultOkErrTypeArgs writtenResult(const NameExpr *name);
  ResultValueGetsUsed checkUsed(const Expr *expr, Discarded how, llvm::SMLoc at);
  ResultStaticIsKnown staticMethod(const NameExpr *name, llvm::StringRef method) const;
  ResultValueGetsUsed checkPromiseUsed(const Expr *expr, Discarded how, llvm::SMLoc at);

  Expr *looselyChecked(Expr *arg);
  void walkArgsLoosely(CallExpr *call);
  Expr *definedHalf(MemberExpr *half);
  bool isGivingType(const Type *half) const;
  const Type *halfParam(const Type *half) const;
  bool isPlainList(const ArrayLitExpr *literal) const;
  std::string receiverText(const Expr *expr) const;
  Expr *checkHalfMember(MemberExpr *member, Half half);
  Expr *checkCombineArray(const CombineCall &combine);
  const Type *expectedResult(const Type *expected) const;
  const Type *halfType(const Type *result, Half half) const;
  const VarSymbol *testedResult(const CallExpr *test) const;
  const Type *otherHalfType(const Type *result, Half half) const;
  const Type *sameValues(const Type *first, const Type *second) const;
  const Type *joinedValue(const Type *first, const Type *second) const;
  const Type *commonResult(llvm::ArrayRef<const Type *> results) const;
  Expr *checkCombineList(const CombineCall &combine, ArrayLitExpr *literal);
  bool isHoldingValues(const CombineCall &combine, const Type *result) const;
  const Type *errorsOf(const CombineCall &combine, const Type *error) const;
  bool hasFittingArgs(CallExpr *call, llvm::SMLoc loc, unsigned min, unsigned max);
  Expr *checkOkErr(CallExpr *call, NameExpr *name, Half half, const Type *expected);
  void reportNoValues(const CombineCall &combine, const Type *result, const Expr *at);
  void splitResultFlows(const VarSymbol *tested, Flow &whenOk, Flow &whenErr) const;
  const Type *checkHalfValue(CallExpr *call, const NameExpr *name, const Type *wantedHalf);
  Expr *checkSameCombine(const CombineCall &combine, ArrayLitExpr *literal, const Type *common);
  Expr *checkTupleCombine(const CombineCall &combine, ArrayLitExpr *literal,
                          const std::vector<const Type *> &results);

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
