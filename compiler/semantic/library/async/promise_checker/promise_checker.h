#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/async/promise_checker/promise_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class Destination;
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

  const Type *resolverType(const Type *value) const;
  const Type *executorValueType(const Expr *executor);
  Expr *checkPromiseNew(NewExpr *expr, const Type *expected);
  ResultPromiseChained checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultValueTypeKnown = Result<const Type *, PromiseCheckerValueTypeError>;
  using ResultExecutorShaped = Result<const Expr *, PromiseCheckerExecutorShapeError>;
  using ResultMethodAccepted = Result<void, PromiseCheckerCheckPromiseMethodError>;
  using ResultThenReturnType = Result<const Type *, PromiseCheckerCheckPromiseMethodError>;

  ResultExecutorShaped checkExecutorShape(const Expr *executor) const;
  ResultMethodAccepted checkIsThen(CallExpr *call, const MemberExpr *member);
  ResultMethodAccepted checkNotResultMethod(CallExpr *call, const MemberExpr *member);
  ResultValueTypeKnown checkValueType(NewExpr *expr, const CallExpr *call, const Type *expected);
  ResultPromiseChained checkChainThen(CallExpr *call, const MemberExpr *member,
                                      const Type *expected);
  ResultThenReturnType checkThenCallback(CallExpr *call, const MemberExpr *member,
                                         const Type *expected);

  Expr *looseExecutor(Expr *executor);
  const Type *withoutPromise(const Type *type) const;
  Expr *typedExecutor(Expr *executor, const Type *value);
  const Type *expectedPromised(const Type *expected) const;
  const FunctionDecl *walkedTrial(const FunctionExpr *functionExpr);
  const Type *joinedValue(const Type *found, const Type *type) const;
  bool isResolvingWithValue(const Stmt *body, llvm::StringRef resolve) const;
  const CallExpr *resolveCall(const Expr *expr, llvm::StringRef resolve) const;
  const Type *resolvedValueType(const Stmt *body, llvm::StringRef resolve) const;
  std::vector<const CallExpr *> resolveCalls(const Stmt *body, llvm::StringRef resolve) const;
  Signature thenSignature(const Expr *callback, const MemberExpr *member,
                          const Type *expected) const;

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  Destination *destination = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClosureChecker *closureChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
