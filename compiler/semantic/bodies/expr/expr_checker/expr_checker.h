#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class NewChecker;
class CallChecker;
class JsonChecker;
class LateClasses;
class NameChecker;
class AsyncChecker;
class LiteralTypes;
class RegexChecker;
class TruthChecker;
class TypeResolver;
class UnaryChecker;
class AssignChecker;
class BinaryChecker;
class MemberChecker;
class ObjectChecker;
class ResultChecker;
class TypeRelations;
class ClosureChecker;
class DecimalChecker;
class GenericChecker;
class LiteralChecker;
class GeneratorChecker;
class ArrayLiteralChecker;
class GenericInstantiator;
class FunctionValueChecker;
class ObjectLiteralChecker;

class ExprChecker {
public:
  ExprChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkExpr(Expr *expr, const Type *expected = nullptr, bool wraps = false);
  Expr *checkExprKind(Expr *expr, const Type *expected, bool wraps);
  Expr *checkSequence(SequenceExpr *seq, const Type *expected, bool wraps);
  Expr *checkOptionalChain(OptionalChainExpr *chain);
  Expr *chainLink(Expr *object);

private:
  using ResultBranchesMatch = Result<Expr *, ExprCheckerCheckConditionalError>;
  using ResultCastSupported = Result<Expr *, ExprCheckerCheckCastError>;

  ResultCastSupported checkCast(CastExpr *cast);
  ResultBranchesMatch checkConditional(ConditionalExpr *cond, const Type *expected);

  SemanticState &state;
  NewChecker *newChecker = nullptr;
  CallChecker *callChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  LateClasses *lateClasses = nullptr;
  NameChecker *nameChecker = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  RegexChecker *regexChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  UnaryChecker *unaryChecker = nullptr;
  AssignChecker *assignChecker = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  ObjectChecker *objectChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  LiteralChecker *literalChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  ArrayLiteralChecker *arrayLiteralChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
  ObjectLiteralChecker *objectLiteralChecker = nullptr;
};

}
