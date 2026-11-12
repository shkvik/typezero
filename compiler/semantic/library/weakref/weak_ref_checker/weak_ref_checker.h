#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class TypeResolver;
class TypeRelations;
class GenericChecker;
class ProcessChecker;
class ArgumentChecker;

class WeakRefChecker {
public:
  WeakRefChecker(SemanticState &owner, SemanticParts &all);

  const Type *resolveWeakRefType(const TypeRef &ref);
  Expr *checkWeakRefNew(NewExpr *expr, const Type *expected);
  Expr *checkWeakRefMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultWeakTargetValid = Result<const Type *, WeakRefCheckerWeakTargetError>;
  using ResultNewTakesOneArgs = Result<const NewExpr *, WeakRefCheckerNewArgsError>;
  using ResultNewTypeArgsFits = Result<const NewExpr *, WeakRefCheckerNewTypeArgsError>;
  using ResultTargetIsWritten = Result<const TypeRef *, WeakRefCheckerTargetWrittenError>;
  using ResultMethodIsDerefed = Result<const MemberExpr *, WeakRefCheckerMethodNameError>;

  ResultNewTakesOneArgs checkNewArgs(const NewExpr *expr) const;
  ResultWeakTargetValid checkWeakTarget(const Type *target) const;
  ResultNewTypeArgsFits checkNewTypeArgs(const NewExpr *expr) const;
  ResultTargetIsWritten checkTargetWritten(const TypeRef &ref) const;
  ResultMethodIsDerefed checkMethodName(const MemberExpr *member, const Type *type) const;
  const Type *writtenTarget(const NewExpr *expr, const Type *expected);
  Expr *failWeakRefNew(NewExpr *expr);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
