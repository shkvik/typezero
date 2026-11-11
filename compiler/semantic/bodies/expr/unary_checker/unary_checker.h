#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/unary_checker/unary_checker_errors.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <optional>

namespace tz {

class SemanticParts;
class DateChecker;
class DictChecker;
class ExprChecker;
class FlowChecker;
class LibraryCalls;
class LiteralTypes;
class TruthChecker;
class TypeRelations;
class ConstantFolder;
class GenericChecker;
class LiteralChecker;
class KeyedFieldChecker;

class UnaryChecker {
public:
  using ResultInOperatorSuited = Result<Expr *, UnaryCheckerCheckInError>;

  UnaryChecker(SemanticState &owner, SemanticParts &all);

  ResultInOperatorSuited checkIn(BinaryExpr *bin);
  Expr *checkUnary(UnaryExpr *unary, const Type *expected, bool wraps);

private:
  using ResultDeleteIsPossible = Result<Expr *, UnaryCheckerCheckDeleteError>;
  using ResultNumberForTheSign = Result<const Type *, UnaryCheckerSignOperandError>;
  using ResultOperandIsNumeric = Result<const Type *, UnaryCheckerNumberOperandError>;
  using ResultOperandIsInteger = Result<const Type *, UnaryCheckerBitsOperandError>;
  using ResultResultNotDropped = Result<const Type *, UnaryCheckerResultKeptError>;
  using ResultNonNullAssertion = Result<const Type *, UnaryCheckerNonNullOperandError>;

  ResultDeleteIsPossible checkDelete(UnaryExpr *unary);
  static ResultDeleteIsPossible
  dictDeleted(Result<Expr *, DictCheckerCheckDictDeleteError> deleted);
  ResultDeleteIsPossible checkMemberDelete(UnaryExpr *unary, MemberExpr *member);
  ResultDeleteIsPossible checkOtherDelete(UnaryExpr *unary);
  ResultDeleteIsPossible checkIndexDelete(UnaryExpr *unary, IndexExpr *index);
  ResultDeleteIsPossible checkKeyDelete(UnaryExpr *unary, const IndexExpr *index,
                                        const std::string &array, const std::string &position);
  Expr *foldedUnary(UnaryExpr *unary, bool wraps);
  Expr *checkSign(UnaryExpr *unary, const Type *expected, bool wraps);
  Expr *checkNegativeLiteral(UnaryExpr *unary, const Type *expected);
  Expr *checkIntLiteral(IntLitExpr *lit, const Type *hint, bool negative);
  static const Type *signHint(const UnaryExpr *unary, const Type *expected);
  Expr *checkNot(UnaryExpr *unary, bool wraps);
  Expr *checkBitNot(UnaryExpr *unary, const Type *expected, bool wraps);
  Expr *checkVoid(UnaryExpr *unary, bool wraps);
  Expr *checkDeleteValue(UnaryExpr *unary);
  Expr *checkNonNull(UnaryExpr *unary, const Type *expected);
  ResultInOperatorSuited checkDictKeyIn(BinaryExpr *bin);
  static UnaryCheckerCheckInErrorCode dictInCode(DictCheckerCheckDictInErrorCode code);
  ResultInOperatorSuited checkUnionIn(BinaryExpr *bin, const Type *r);
  static ResultInOperatorSuited unionKeyIn(BinaryExpr *bin, const Type *u, const Type *objects,
                                           const std::string &name);
  ResultInOperatorSuited checkObjectIn(BinaryExpr *bin, const Type *object);
  static ResultOperandIsInteger bitsOperand(const Type *type);
  static std::optional<std::string> simpleText(const Expr *expr);
  static std::optional<std::string> assertedText(const Expr *expr);
  static NodeChildren<const Expr *> textParts(const Expr *expr);
  static std::optional<std::string> nodeText(const Expr *expr, std::vector<std::string> &answers);
  static ResultNumberForTheSign signOperand(llvm::StringRef op, const Type *type);
  static ResultNonNullAssertion nonNullOperand(const Type *type, llvm::StringRef text);
  static ResultDeleteIsPossible fieldDeleted(const Expr *object, const Field *field);
  ResultResultNotDropped resultKept(const UnaryExpr *unary, const Type *dropped,
                                    bool promised) const;
  static ResultOperandIsNumeric numberOperand(const UnaryExpr *unary, llvm::StringRef op,
                                              const Type *type, bool literal);

  SemanticState &state;
  DateChecker *dateChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TruthChecker *truthChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  LiteralChecker *literalChecker = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
