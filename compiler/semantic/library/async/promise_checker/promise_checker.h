#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/async/promise_checker/promise_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class FlowChecker;
class TypeResolver;
class ClosureChecker;
class ReturnInferrer;
class ArgumentChecker;
class CallbackChecker;

class PromiseChecker {
public:
  using ResultPromiseChained = Result<Expr *, PromiseCheckerCheckPromiseMethodError>;

  PromiseChecker(SemanticState &owner, SemanticParts &all);

  static const Type *resolverType(const Type *value);
  const Type *executorValueType(const Expr *executor);
  Expr *checkPromiseNew(NewExpr *expr, const Type *expected);
  ResultPromiseChained checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultValueTypeKnown = Result<const Type *, PromiseCheckerValueTypeError>;
  using ResultExecutorShaped = Result<const Expr *, PromiseCheckerExecutorShapeError>;

  static const Type *awaited(const Type *type);
  static bool callsWithValue(const Stmt *body, llvm::StringRef resolve);
  static ResultExecutorShaped executorShape(const Expr *executor);
  ResultValueTypeKnown valueType(NewExpr *expr, const CallExpr *call, const Type *expected);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClosureChecker *closureChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
