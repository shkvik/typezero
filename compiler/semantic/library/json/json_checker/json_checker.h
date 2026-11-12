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
class BinaryChecker;
class ArgumentChecker;
class ArrayLiteralChecker;

class JsonChecker {
public:
  JsonChecker(SemanticState &owner, SemanticParts &all);

  bool holdsJson(const Type *type) const;
  bool isNullName(const Expr *expr) const;
  Expr *checkNull(NameExpr *name, const Type *expected);
  Expr *checkJsonMember(MemberExpr *member);
  Expr *checkJsonIndex(IndexExpr *index);
  const Type *checkJsonTarget(Expr *target);
  Expr *reportJsonMethod(CallExpr *call, MemberExpr *member);
  Expr *checkJsonObjectLit(ObjectLitExpr *literal);
  Expr *checkJsonArrayLit(ArrayLitExpr *literal);
  Expr *checkNullEquality(BinaryExpr *binary);
  Expr *checkJsonEquality(BinaryExpr *binary);
  void addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  std::optional<FieldPath> jsonField(const Expr *expr) const;
  void narrowJsonField(Flow &flow, const FieldPath &path, const Type *type) const;
  Expr *checkArrayIsArray(CallExpr *call, MemberExpr *member);
  Expr *checkJsonIsJson(CallExpr *call, MemberExpr *member);
  void walkRedundantTests(const BinaryExpr *binary);

private:
  using ResultTypeOfAgrees = Result<const Expr *, JsonCheckerTypeOfAgreesError>;
  using ResultNullPossible = Result<const Type *, JsonCheckerNullPossibleError>;
  using ResultTestsUnknown = Result<const Type *, JsonCheckerTestsUnknownError>;

  ResultTypeOfAgrees typeOfAgrees(const Expr *tested, llvm::StringRef name, bool nullTest,
                                  bool isAnd) const;
  ResultTestsUnknown testsUnknown(const Type *type);
  ResultNullPossible nullPossible(const Type *type, llvm::StringRef op) const;
  void addArrayFacts(const Expr *arg, Flow &whenTrue) const;
  void addNothingFacts(const Expr *side, const Expr *other, Flow &equal, Flow &different) const;
  void addTypeOfFacts(const Expr *operand, const std::u16string &text, Flow &equal,
                      Flow &different) const;
  const Type *typeOfJson(const std::u16string &text) const;
  const VarSymbol *jsonVariable(const Expr *expr) const;
  bool nullName(const Expr *expr) const;
  bool isNothing(const Expr *expr) const;
  const Expr *bare(const Expr *expr) const;
  bool sameRef(const Expr *a, const Expr *b) const;
  std::string refText(const Expr *expr) const;

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  ArrayLiteralChecker *arrayLiteralChecker = nullptr;
};

}
