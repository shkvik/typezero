#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker_errors.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker_errors.h"
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
  static ExprCheckerCheckCastErrorCode castProblem(const Expr *operand, const Type *to,
                                                   bool arrayLiteral);
  static std::string castMessage(ExprCheckerCheckCastErrorCode code, const Expr *operand,
                                 const Type *to);
  static std::string hintedCastMessage(const Expr *operand, const Type *to);
  ResultBranchesMatch checkConditional(ConditionalExpr *cond, const Type *expected);
  static std::optional<Signature> expectedSignature(const Type *expected);
  Expr *checkFunctionValue(Expr *expr, const Type *expected);
  Expr *checkIntLiteral(IntLitExpr *lit, const Type *expected);
  Expr *checkFloatLiteral(FloatLitExpr *lit, const Type *expected);
  Expr *checkBoolLiteral(BoolLitExpr *lit, const Type *expected);
  Expr *checkStringLiteral(StringLitExpr *lit, const Type *expected);
  Expr *checkNameValue(NameExpr *name, const Type *expected);
  Expr *checkMemberValue(MemberExpr *member, const Type *expected);
  Expr *checkIndexValue(IndexExpr *index);
  static llvm::SMLoc indexErrorLoc(const IndexExpr *index, MemberCheckerCheckIndexErrorCode code);
  Expr *checkConditionalValue(ConditionalExpr *conditional, const Type *expected);
  Expr *checkCastValue(CastExpr *castExpr);
  Expr *checkFunctionLiteral(FunctionExpr *fn, const Type *expected);

  struct ConditionFlows {
    Flow whenTrue;
    Flow whenFalse;
  };

  static bool waitsForOther(const Expr *branch);
  static const Type *branchHint(const Expr *literal, const Expr *other, const Type *expected);
  void walkElseFirst(ConditionalExpr *cond, const Type *expected, const ConditionFlows &flows);
  void walkThenFirst(ConditionalExpr *cond, const Type *expected, const ConditionFlows &flows,
                     bool elseWaitsOnly);
  ResultBranchesMatch joinBranches(ConditionalExpr *cond);

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
