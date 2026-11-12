#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker_errors.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
class Destination;
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

  Expr *checkExpr(Expr *expr, const Type *expected = nullptr);
  Expr *checkExprKind(Expr *expr, const Type *expected);
  Expr *checkSequence(SequenceExpr *sequence, const Type *expected);
  Expr *checkOptionalChain(OptionalChainExpr *chain);
  Expr *chainLink(Expr *object);
  Expr *shortCircuitLink(Expr *object, const Type *type);

private:
  using ResultBranchesMatch = Result<Expr *, ExprCheckerCheckConditionalError>;
  using ResultCastSupported = Result<Expr *, ExprCheckerCheckCastError>;

  ResultCastSupported checkCast(CastExpr *castExpr);
  ExprCheckerCheckCastErrorCode castProblem(const Expr *operand, const Type *to,
                                            bool isArrayLiteral) const;
  std::string castMessage(ExprCheckerCheckCastErrorCode code, const Expr *operand,
                          const Type *to) const;
  std::string hintedCastMessage(const Expr *operand, const Type *to) const;
  ResultBranchesMatch checkConditional(ConditionalExpr *conditional, const Type *expected);
  std::optional<Signature> expectedSignature(const Type *expected) const;
  Expr *checkFunctionValue(Expr *expr, const Type *expected);
  Expr *checkIntLiteral(IntLitExpr *literal, const Type *expected);
  Expr *checkFloatLiteral(FloatLitExpr *literalExpr, const Type *expected);
  Expr *checkBoolLiteral(BoolLitExpr *literalExpr, const Type *expected);
  Expr *checkStringLiteral(StringLitExpr *literalExpr, const Type *expected);
  Expr *checkNameValue(NameExpr *name, const Type *expected);
  Expr *checkMemberValue(MemberExpr *member, const Type *expected);
  Expr *checkIndexValue(IndexExpr *index);
  llvm::SMLoc indexErrorLoc(const IndexExpr *index, MemberCheckerCheckIndexErrorCode code) const;
  Expr *checkConditionalValue(ConditionalExpr *conditional, const Type *expected);
  Expr *checkCastValue(CastExpr *castExpr);
  Expr *checkFunctionLiteral(FunctionExpr *functionExpr, const Type *expected);

  struct ConditionFlows {
    Flow whenTrue;
    Flow whenFalse;
  };

  bool isWaitingForOther(const Expr *branch) const;
  const Type *elseHint(const ConditionalExpr *conditional, const Type *expected,
                       bool isElseWaitingOnly) const;
  const Type *failedBranchesType(const Type *thenType, const Type *elseType) const;
  const Type *branchHint(const Expr *literal, const Expr *other, const Type *expected) const;
  void walkElseFirst(ConditionalExpr *conditional, const Type *expected,
                     const ConditionFlows &flows);
  void walkThenFirst(ConditionalExpr *conditional, const Type *expected,
                     const ConditionFlows &flows, bool isElseWaitingOnly);
  ResultBranchesMatch joinBranches(ConditionalExpr *conditional);

  SemanticState &state;
  NewChecker *newChecker = nullptr;
  CallChecker *callChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  LateClasses *lateClasses = nullptr;
  NameChecker *nameChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  Destination *destination = nullptr;
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
