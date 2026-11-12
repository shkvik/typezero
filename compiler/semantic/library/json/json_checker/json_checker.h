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

  bool isHoldingJson(const Type *type) const;
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

  ResultTypeOfAgrees checkTypeOfAgrees(const Expr *tested, llvm::StringRef name, bool nullTest,
                                       bool isAnd) const;
  ResultTestsUnknown checkTestsUnknown(const Type *type);
  ResultNullPossible checkNullPossible(const Type *type, llvm::StringRef op) const;
  void addArrayFacts(const Expr *arg, Flow &whenTrue) const;
  void addNothingFacts(const Expr *side, const Expr *other, Flow &equal, Flow &different) const;
  void addTypeOfFacts(const Expr *operand, const std::u16string &text, Flow &equal,
                      Flow &different) const;
  const Type *typeOfJson(const std::u16string &text) const;
  const VarSymbol *jsonVariable(const Expr *expr) const;
  bool isNothing(const Expr *expr) const;
  const Expr *bare(const Expr *expr) const;
  bool isSameRef(const Expr *first, const Expr *second) const;
  std::string refText(const Expr *expr) const;

  struct JsonTest {
    const Expr *tested;
    bool isNullTest;
  };

  struct TypeOfPair {
    const UnaryExpr *typeOf;
    const StringLitExpr *text;
  };

  std::optional<JsonTest> jsonTest(const Expr *rhs) const;
  std::optional<JsonTest> testedSide(const Expr *rhs) const;
  bool isNullComparison(const BinaryExpr *equality) const;
  const Expr *withoutNot(const Expr *expr) const;
  bool reportRedundantTest(const Expr *term, const JsonTest &test, bool isAnd, const Expr *rhs);
  const StringLitExpr *typeOfTextFor(const Expr *term, const Expr *tested, bool isAnd) const;
  TypeOfPair typeOfPair(const BinaryExpr *comparison) const;
  void walkJsonProperty(ObjectLitExpr::Property &property, const Type *json);
  void walkJsonKey(ObjectLitExpr::Property &property, ComputedProperty *computed);
  bool isNarrowedPath(const FieldPath &path, const std::pair<FieldPath, const Type *> &entry) const;
  void addSideFacts(const Expr *side, const Expr *other, Flow &equal, Flow &different) const;

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
