#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/literal_checker/literal_checker.h"
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
class BinaryChecker;
class ObjectChecker;
class ConstantFolder;
class GenericChecker;
class LiteralChecker;
class KeyedFieldChecker;

class UnaryChecker {
public:
  using ResultInOperatorSuited = Result<Expr *, UnaryCheckerCheckInError>;

  UnaryChecker(SemanticState &owner, SemanticParts &all);

  ResultInOperatorSuited checkIn(BinaryExpr *binary);
  Expr *checkUnary(UnaryExpr *unary, const Type *expected);

private:
  using ResultDeleteIsPossible = Result<Expr *, UnaryCheckerCheckDeleteError>;
  using ResultNumberForTheSign = Result<const Type *, UnaryCheckerSignOperandError>;
  using ResultOperandIsNumeric = Result<const Type *, UnaryCheckerNumberOperandError>;
  using ResultOperandIsInteger = Result<const Type *, UnaryCheckerBitsOperandError>;
  using ResultResultNotDropped = Result<const Type *, UnaryCheckerResultKeptError>;
  using ResultNonNullAssertion = Result<const Type *, UnaryCheckerNonNullOperandError>;

  std::optional<std::string> simpleText(const Expr *expr) const;
  std::optional<std::string> assertedText(const Expr *expr) const;
  NodeChildren<const Expr *> textParts(const Expr *expr) const;
  std::optional<std::string> nodeText(const Expr *expr, std::vector<std::string> &answers) const;
  std::string memberText(const MemberExpr *member, std::vector<std::string> &answers) const;
  std::string indexText(std::vector<std::string> &answers) const;
  ResultNumberForTheSign checkSignOperand(llvm::StringRef op, const Type *type) const;
  ResultOperandIsNumeric checkNumberOperand(const UnaryExpr *unary, llvm::StringRef op,
                                            const Type *type, bool isLiteral) const;
  ResultOperandIsInteger checkBitsOperand(const Type *type) const;
  ResultResultNotDropped checkResultKept(const UnaryExpr *unary, const Type *dropped,
                                         bool isPromised) const;
  ResultNonNullAssertion checkNonNullOperand(const Type *type, llvm::StringRef text) const;
  Expr *checkTypeOf(UnaryExpr *unary);
  Expr *foldedUnary(UnaryExpr *unary);
  Expr *checkSign(UnaryExpr *unary, const Type *expected);
  void walkSignOperand(UnaryExpr *unary, const Type *hint);
  const Type *signOperandType(UnaryExpr *unary);
  Expr *checkNegativeLiteral(UnaryExpr *unary, const Type *expected);
  Expr *checkNegativeInteger(UnaryExpr *unary, IntLitExpr *integer, const Type *expected);
  Expr *checkNegativeFraction(UnaryExpr *unary, FloatLitExpr *real, const Type *expected);
  Expr *checkIntLiteral(IntLitExpr *literal, const Type *hint, LiteralSign sign);
  const Type *signHint(const UnaryExpr *unary, const Type *expected) const;
  Expr *checkNot(UnaryExpr *unary);
  Expr *checkBitNot(UnaryExpr *unary, const Type *expected);
  Expr *checkVoid(UnaryExpr *unary);
  Expr *checkDeleteValue(UnaryExpr *unary);
  Expr *checkNonNull(UnaryExpr *unary, const Type *expected);
  ResultDeleteIsPossible checkFieldDelete(const Expr *object, const Field *field) const;
  ResultDeleteIsPossible checkDelete(UnaryExpr *unary);
  ResultDeleteIsPossible
  dictDeleteResult(Result<Expr *, DictCheckerCheckDictDeleteError> deleted) const;
  ResultDeleteIsPossible checkMemberDelete(UnaryExpr *unary, MemberExpr *member);
  const Field *ownField(const Type *object, llvm::StringRef name) const;
  ResultDeleteIsPossible checkOtherDelete(UnaryExpr *unary);
  ResultDeleteIsPossible checkIndexDelete(UnaryExpr *unary, IndexExpr *index);
  ResultDeleteIsPossible checkKeyDelete(UnaryExpr *unary, const IndexExpr *index,
                                        const std::string &array, const std::string &position);
  const Field *keyedField(const Type *type, const IndexExpr *index) const;
  std::string inBoundsText(const std::string &indexText, const std::string &array,
                           bool isLiteralKey) const;
  ResultInOperatorSuited checkDictKeyIn(BinaryExpr *binary);
  UnaryCheckerCheckInErrorCode dictInCode(DictCheckerCheckDictInErrorCode code) const;
  ResultInOperatorSuited checkUnionIn(BinaryExpr *binary, const Type *rightType);
  std::vector<const Type *> objectMembers(const Type *objects) const;
  ResultInOperatorSuited checkUnionKeyIn(BinaryExpr *binary, const Type *options,
                                         const Type *objects, const std::string &name) const;
  ResultInOperatorSuited checkFieldRequired(BinaryExpr *binary, const Type *member,
                                            const std::string &name) const;
  size_t countWithField(llvm::ArrayRef<const Type *> members, const std::string &name) const;
  ResultInOperatorSuited checkObjectIn(BinaryExpr *binary, const Type *object);

  SemanticState &state;
  DateChecker *dateChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TruthChecker *truthChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  ObjectChecker *objectChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  LiteralChecker *literalChecker = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
