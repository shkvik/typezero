#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/json/json_checker/json_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FieldFacts;
class CallChecker;
class ExprChecker;
class TypeRelations;
class ArgumentChecker;
class ArrayLiteralChecker;

class JsonChecker {
public:
  JsonChecker(SemanticState &owner, SemanticParts &all);

  static bool holdsJson(const Type *type);
  static bool isNullName(const Expr *expr);
  Expr *checkNull(NameExpr *name, const Type *expected);
  Expr *checkJsonMember(MemberExpr *member);
  Expr *checkJsonIndex(IndexExpr *index);
  const Type *checkJsonTarget(Expr *target);
  Expr *reportJsonMethod(CallExpr *call, MemberExpr *member);
  Expr *checkJsonObjectLit(ObjectLitExpr *lit);
  Expr *checkJsonArrayLit(ArrayLitExpr *lit);
  Expr *checkNullEquality(BinaryExpr *bin);
  Expr *checkJsonEquality(BinaryExpr *bin);
  void addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  std::optional<FieldPath> jsonField(const Expr *expr) const;
  static void narrowJsonField(Flow &flow, const FieldPath &path, const Type *type);
  Expr *checkArrayIsArray(CallExpr *call, MemberExpr *member);
  Expr *checkJsonIsJson(CallExpr *call, MemberExpr *member);
  void checkRedundantTests(const BinaryExpr *bin);

private:
  using ResultTypeOfAgrees = Result<const Expr *, JsonCheckerTypeOfAgreesError>;
  using ResultNullPossible = Result<const Type *, JsonCheckerNullPossibleError>;
  using ResultTestsUnknown = Result<const Type *, JsonCheckerTestsUnknownError>;

  static ResultTypeOfAgrees typeOfAgrees(const Expr *tested, llvm::StringRef name, bool nullTest,
                                         bool isAnd);
  ResultTestsUnknown testsUnknown(const Type *type);
  static ResultNullPossible nullPossible(const Type *type, llvm::StringRef op);
  static const VarSymbol *jsonVariable(const Expr *expr);
  static bool nullName(const Expr *expr);
  static bool isNothing(const Expr *expr);
  static const Expr *bare(const Expr *expr);
  static bool sameRef(const Expr *a, const Expr *b);
  static std::string refText(const Expr *expr);

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  ArrayLiteralChecker *arrayLiteralChecker = nullptr;
};

}
