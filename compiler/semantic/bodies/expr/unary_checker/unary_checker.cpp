#include "compiler/semantic/bodies/expr/unary_checker/unary_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/literal_checker/literal_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";
constexpr const char *kUnnamedValue = "x";
constexpr const char *kUnnamedArray = "a";
constexpr const char *kUnnamedIndex = "i";

}

UnaryChecker::UnaryChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dateChecker = &all.dateChecker;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->libraryCalls = &all.libraryCalls;
  this->literalTypes = &all.literalTypes;
  this->truthChecker = &all.truthChecker;
  this->typeRelations = &all.typeRelations;
  this->binaryChecker = &all.binaryChecker;
  this->objectChecker = &all.objectChecker;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->literalChecker = &all.literalChecker;
  this->keyedFieldChecker = &all.keyedFieldChecker;
}

std::optional<std::string> UnaryChecker::simpleText(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *literal = dyn_cast<IntLitExpr>(expr);
  if (name != nullptr) {
    return name->name.str();
  }
  if (literal != nullptr) {
    return std::to_string(literal->value);
  }

  return std::nullopt;
}

std::optional<std::string> UnaryChecker::assertedText(const Expr *expr) const {
  auto answers = std::vector<std::string>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &UnaryChecker::textParts)) {
    auto text = this->nodeText(node, answers);
    if (!text.has_value()) {
      return std::nullopt;
    }
    answers.push_back(*text);
  }

  return answers.back();
}

NodeChildren<const Expr *> UnaryChecker::textParts(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    return {conversion->operand};
  }
  if (member != nullptr) {
    return {member->object};
  }
  if (index != nullptr) {
    return {index->object, index->index};
  }

  return {};
}

std::optional<std::string> UnaryChecker::nodeText(const Expr *expr,
                                                  std::vector<std::string> &answers) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  if (conversion != nullptr && !conversion->isImplicit()) {
    return std::nullopt;
  }
  if (conversion != nullptr) {
    return ChildrenFirst::takeAnswer(answers);
  }
  if (member != nullptr) {
    return this->memberText(member, answers);
  }
  if (isa<IndexExpr>(expr)) {
    return this->indexText(answers);
  }
  if (name != nullptr && name->name.starts_with(kHiddenPrefix)) {
    return std::nullopt;
  }

  return this->simpleText(expr);
}

std::string UnaryChecker::memberText(const MemberExpr *member,
                                     std::vector<std::string> &answers) const {
  auto object = ChildrenFirst::takeAnswer(answers);
  const char *dot = member->optional ? "?." : ".";

  return object + dot + member->name.str();
}

std::string UnaryChecker::indexText(std::vector<std::string> &answers) const {
  auto position = ChildrenFirst::takeAnswer(answers);
  auto object = ChildrenFirst::takeAnswer(answers);

  return object + "[" + position + "]";
}

UnaryChecker::ResultNumberForTheSign UnaryChecker::checkSignOperand(llvm::StringRef op,
                                                                    const Type *type) const {
  if (type->isString() || type->isBool()) {
    auto message = ErrorTextUnary::noNumber(op, type->isString());

    return err(UnaryCheckerSignOperandError{
        .code = UnaryCheckerSignOperandErrorCode::STRING_OR_BOOL,
        .message = message,
    });
  }

  return ok(type);
}

UnaryChecker::ResultOperandIsNumeric UnaryChecker::checkNumberOperand(const UnaryExpr *unary,
                                                                      llvm::StringRef op,
                                                                      const Type *type,
                                                                      bool isLiteral) const {
  if (type->isDecimal() && unary->op == UnaryOp::Plus && !isLiteral) {
    auto message = ErrorTextUnary::decimalSum();

    return err(UnaryCheckerNumberOperandError{
        .code = UnaryCheckerNumberOperandErrorCode::DECIMAL_PLUS,
        .message = message,
    });
  }
  if (!type->isNumeric() && !type->isDecimal() && !type->isError()) {
    auto message = ErrorTextUnary::notNumber(op, type->name());

    return err(UnaryCheckerNumberOperandError{
        .code = UnaryCheckerNumberOperandErrorCode::NOT_NUMERIC,
        .message = message,
    });
  }

  return ok(type);
}

UnaryChecker::ResultOperandIsInteger UnaryChecker::checkBitsOperand(const Type *type) const {
  if (type->isFloat()) {
    auto message = ErrorTextUnary::bitsDouble();

    return err(UnaryCheckerBitsOperandError{
        .code = UnaryCheckerBitsOperandErrorCode::DOUBLE_BITS,
        .message = message,
    });
  }
  if (!type->isInteger() && !type->isError()) {
    auto message = ErrorTextUnary::bitsType(type->name());

    return err(UnaryCheckerBitsOperandError{
        .code = UnaryCheckerBitsOperandErrorCode::NOT_INTEGER,
        .message = message,
    });
  }

  return ok(type);
}

UnaryChecker::ResultResultNotDropped
UnaryChecker::checkResultKept(const UnaryExpr *unary, const Type *dropped, bool isPromised) const {
  bool isLibrary = this->libraryCalls->isInLibrary(unary->loc());
  if (dropped->holdsResults() && !isLibrary) {
    std::string message = isPromised ? ErrorTextUnary::dropsAsync() : ErrorTextUnary::drops();

    return err(UnaryCheckerResultKeptError{
        .code = UnaryCheckerResultKeptErrorCode::DROPS_RESULT,
        .message = message,
    });
  }

  return ok(dropped);
}

UnaryChecker::ResultNonNullAssertion UnaryChecker::checkNonNullOperand(const Type *type,
                                                                       llvm::StringRef text) const {
  if (type->isUndefined()) {
    auto message = ErrorTextUnary::allUndef();

    return err(UnaryCheckerNonNullOperandError{
        .code = UnaryCheckerNonNullOperandErrorCode::ALWAYS_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isOptional()) {
    auto message = ErrorTextUnary::neverUndef(text);

    return err(UnaryCheckerNonNullOperandError{
        .code = UnaryCheckerNonNullOperandErrorCode::NEVER_UNDEFINED,
        .message = message,
    });
  }
  auto message = ErrorTextUnary::checkFirst(text);

  return err(UnaryCheckerNonNullOperandError{
      .code = UnaryCheckerNonNullOperandErrorCode::NOT_SUPPORTED,
      .message = message,
  });
}

Expr *UnaryChecker::checkUnary(UnaryExpr *unary, const Type *expected) {
  switch (unary->op) {
    case UnaryOp::Neg:
    case UnaryOp::Plus:
      return this->checkSign(unary, expected);
    case UnaryOp::Not:
      return this->checkNot(unary);
    case UnaryOp::BitNot:
      return this->checkBitNot(unary, expected);
    case UnaryOp::TypeOf:
      return this->checkTypeOf(unary);
    case UnaryOp::Void:
      return this->checkVoid(unary);
    case UnaryOp::Delete:
      return this->checkDeleteValue(unary);
    case UnaryOp::NonNull:
      return this->checkNonNull(unary, expected);
  }

  return unary;
}

Expr *UnaryChecker::checkTypeOf(UnaryExpr *unary) {
  unary->operand = this->exprChecker->checkExpr(unary->operand);
  unary->type = Type::string();

  return this->foldedUnary(unary);
}

Expr *UnaryChecker::foldedUnary(UnaryExpr *unary) {
  if (unary->type != nullptr && unary->type->isInteger()) {
    this->constantFolder->foldUnary(unary);
  }

  return unary;
}

Expr *UnaryChecker::checkSign(UnaryExpr *unary, const Type *expected) {
  auto *negative =
      unary->op == UnaryOp::Neg ? this->checkNegativeLiteral(unary, expected) : nullptr;
  if (negative != nullptr) {
    return negative;
  }
  const auto *hint = this->signHint(unary, expected);
  this->walkSignOperand(unary, hint);
  const auto *type = this->signOperandType(unary);
  const char *op = Operators::opSpelling(unary->op);
  auto sign = this->checkSignOperand(op, type);
  if (sign.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), sign.error().message);
    unary->type = Type::error();
    return this->foldedUnary(unary);
  }
  if (type->isDate() && unary->op == UnaryOp::Plus) {
    return this->dateChecker->dateTime(unary->operand);
  }
  bool isLiteral = isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand);
  auto numeric = this->checkNumberOperand(unary, op, type, isLiteral);
  if (numeric.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), numeric.error().message);
  }
  unary->type = numeric.isOk() ? type : Type::error();

  return this->foldedUnary(unary);
}

void UnaryChecker::walkSignOperand(UnaryExpr *unary, const Type *hint) {
  auto *literal = dyn_cast<IntLitExpr>(unary->operand);
  if (literal != nullptr) {
    auto sign = unary->op == UnaryOp::Neg ? LiteralSign::NEGATED : LiteralSign::PLAIN;
    unary->operand = this->checkIntLiteral(literal, hint, sign);
    return;
  }
  auto *checked = this->exprChecker->checkExpr(unary->operand, hint);
  unary->operand = this->genericChecker->apparent(checked);
}

const Type *UnaryChecker::signOperandType(UnaryExpr *unary) {
  const Type *type = unary->operand->type;
  if (!type->isNever()) {
    return type;
  }
  unary->operand = this->typeRelations->convert(unary->operand, Type::doubleTy());

  return Type::doubleTy();
}

Expr *UnaryChecker::checkNegativeLiteral(UnaryExpr *unary, const Type *expected) {
  auto *integer = dyn_cast<IntLitExpr>(unary->operand);
  auto *real = dyn_cast<FloatLitExpr>(unary->operand);
  if (integer != nullptr) {
    return this->checkNegativeInteger(unary, integer, expected);
  }
  if (real != nullptr) {
    return this->checkNegativeFraction(unary, real, expected);
  }

  return nullptr;
}

Expr *UnaryChecker::checkNegativeInteger(UnaryExpr *unary, IntLitExpr *integer,
                                         const Type *expected) {
  auto value = -double(integer->value);
  const auto *literal =
      this->literalTypes->numberLiteralFor(expected, value, NumberSpelling::WHOLE);
  if (literal == nullptr || integer->value == 0) {
    return nullptr;
  }
  unary->operand = this->checkIntLiteral(integer, literal->literalBase(), LiteralSign::NEGATED);
  unary->type = literal;

  return unary;
}

Expr *UnaryChecker::checkNegativeFraction(UnaryExpr *unary, FloatLitExpr *real,
                                          const Type *expected) {
  const auto *literal =
      this->literalTypes->numberLiteralFor(expected, -real->value, NumberSpelling::FRACTION);
  if (literal == nullptr) {
    return nullptr;
  }
  real->type = Type::doubleTy();
  unary->type = literal;

  return unary;
}

Expr *UnaryChecker::checkIntLiteral(IntLitExpr *literal, const Type *hint, LiteralSign sign) {
  auto typed = this->literalChecker->checkIntLit(literal, hint, sign);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), typed.error().message);
    return literal;
  }

  return typed.value();
}

const Type *UnaryChecker::signHint(const UnaryExpr *unary, const Type *expected) const {
  bool isFraction = isa<FloatLitExpr>(unary->operand);
  auto spelling = isFraction ? NumberSpelling::FRACTION : NumberSpelling::WHOLE;
  const auto *member = this->literalTypes->numberMemberOf(expected, spelling);
  const Type *plain = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *wanted = member != nullptr ? member : plain;
  bool isNumeric = wanted != nullptr && (wanted->isNumeric() || wanted->isDecimal());

  return isNumeric ? wanted : nullptr;
}

Expr *UnaryChecker::checkNot(UnaryExpr *unary) {
  auto whenTrue = Flow();
  auto whenFalse = Flow();
  this->truthChecker->checkBranching(unary, whenTrue, whenFalse);
  auto joined = this->flowChecker->join(whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(joined);

  return this->foldedUnary(unary);
}

Expr *UnaryChecker::checkBitNot(UnaryExpr *unary, const Type *expected) {
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *hint = wanted != nullptr && wanted->isInteger() ? wanted : nullptr;
  auto *checked = this->exprChecker->checkExpr(unary->operand, hint);
  unary->operand = this->genericChecker->apparent(checked);
  const Type *type = unary->operand->type;
  auto integer = this->checkBitsOperand(type);
  if (integer.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), integer.error().message);
  }
  unary->type = integer.isOk() ? type : Type::error();

  return this->foldedUnary(unary);
}

Expr *UnaryChecker::checkVoid(UnaryExpr *unary) {
  unary->operand = this->exprChecker->checkExpr(unary->operand);
  unary->type = Type::undefinedTy();
  const Type *operandType = unary->operand->type;
  bool isPromised = operandType->isPromise();
  const Type *dropped = isPromised ? operandType->promised() : operandType;
  auto kept = this->checkResultKept(unary, dropped, isPromised);
  if (kept.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), kept.error().message);
  }

  return this->foldedUnary(unary);
}

Expr *UnaryChecker::checkDeleteValue(UnaryExpr *unary) {
  auto deleted = this->checkDelete(unary);
  if (deleted.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), deleted.error().message);
    return unary;
  }

  return deleted.value();
}

Expr *UnaryChecker::checkNonNull(UnaryExpr *unary, const Type *expected) {
  unary->operand = this->exprChecker->checkExpr(unary->operand, expected);
  const Type *type = unary->operand->type;
  auto text = this->assertedText(unary->operand).value_or(kUnnamedValue);
  unary->type = type->isUndefined() ? Type::error() : type->nonOptional();
  if (type->isError()) {
    return unary;
  }
  auto asserted = this->checkNonNullOperand(type, text);
  if (asserted.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), asserted.error().message);
  }
  bool isRefused = asserted.isErr() &&
                   asserted.error().code != UnaryCheckerNonNullOperandErrorCode::NOT_SUPPORTED;
  if (isRefused) {
    return unary;
  }
  auto *name = dyn_cast<NameExpr>(unary->operand);
  if (name != nullptr && name->var() != nullptr) {
    this->flowChecker->markDefined(name->var());
  }

  return unary;
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::checkFieldDelete(const Expr *object,
                                                                    const Field *field) const {
  if (field->type->isOptional()) {
    auto text = this->objectChecker->fieldText(object, field->name);
    auto message = ErrorTextDelete::optional(field->name, text);

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::OPTIONAL_FIELD,
        .message = message,
    });
  }
  auto message = ErrorTextDelete::required(field->name);

  return err(UnaryCheckerCheckDeleteError{
      .code = UnaryCheckerCheckDeleteErrorCode::REQUIRED_FIELD,
      .message = message,
  });
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::checkDelete(UnaryExpr *unary) {
  unary->type = Type::boolTy();
  auto *index = dyn_cast<IndexExpr>(unary->operand);
  auto *member = dyn_cast<MemberExpr>(unary->operand);
  if (member != nullptr && !member->optional) {
    return this->checkMemberDelete(unary, member);
  }
  if (index == nullptr) {
    return this->checkOtherDelete(unary);
  }

  return this->checkIndexDelete(unary, index);
}

UnaryChecker::ResultDeleteIsPossible
UnaryChecker::dictDeleteResult(Result<Expr *, DictCheckerCheckDictDeleteError> deleted) const {
  if (deleted.isErr()) {
    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::DICT_DELETE,
        .message = deleted.error().message,
    });
  }

  return ok(deleted.value());
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::checkMemberDelete(UnaryExpr *unary,
                                                                     MemberExpr *member) {
  member->object = this->exprChecker->checkExpr(member->object);
  const Type *object = member->object->type;
  if (object->isDict()) {
    auto deleted = this->dictChecker->checkDictDelete(unary, member->object, nullptr, member->name);
    return this->dictDeleteResult(deleted);
  }
  if (object->isError()) {
    return ok(unary);
  }
  const auto *field = this->ownField(object, member->name);
  if (field != nullptr) {
    return this->checkFieldDelete(member->object, field);
  }
  auto message = ErrorTextDelete::notField();

  return err(UnaryCheckerCheckDeleteError{
      .code = UnaryCheckerCheckDeleteErrorCode::NOT_A_FIELD,
      .message = message,
  });
}

const Field *UnaryChecker::ownField(const Type *object, llvm::StringRef name) const {
  const Type *own = object->nonOptional();
  if (!own->isObject()) {
    return nullptr;
  }

  return own->field(name);
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::checkOtherDelete(UnaryExpr *unary) {
  unary->operand = this->exprChecker->checkExpr(unary->operand);
  const Type *type = unary->operand->type;
  auto *member = dyn_cast<MemberExpr>(unary->operand);
  if (type->isError()) {
    return ok(unary);
  }
  const auto *field =
      member != nullptr ? this->ownField(member->object->type, member->name) : nullptr;
  if (field != nullptr) {
    return this->checkFieldDelete(member->object, field);
  }
  auto message = ErrorTextDelete::notField();

  return err(UnaryCheckerCheckDeleteError{
      .code = UnaryCheckerCheckDeleteErrorCode::NOT_A_FIELD,
      .message = message,
  });
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::checkIndexDelete(UnaryExpr *unary,
                                                                    IndexExpr *index) {
  auto array = this->simpleText(index->object).value_or(kUnnamedArray);
  auto position = this->simpleText(index->index).value_or(kUnnamedIndex);
  index->object = this->exprChecker->checkExpr(index->object);
  if (index->object->type->isDict()) {
    auto deleted = this->dictChecker->checkDictDelete(unary, index->object, index->index, {});
    return this->dictDeleteResult(deleted);
  }
  index->index = this->exprChecker->checkExpr(index->index);
  index->type = Type::error();

  return this->checkKeyDelete(unary, index, array, position);
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::checkKeyDelete(UnaryExpr *unary,
                                                                  const IndexExpr *index,
                                                                  const std::string &array,
                                                                  const std::string &position) {
  const Type *type = index->object->type->nonOptional();
  if (type->isArray()) {
    auto message = ErrorTextDelete::hole(array, position);

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::ARRAY_HOLE,
        .message = message,
    });
  }
  if (type->isString()) {
    auto message = ErrorTextDelete::character();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::STRING_CHARACTER,
        .message = message,
    });
  }
  if (type->isTuple()) {
    auto message = ErrorTextDelete::tupleHole();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::TUPLE_HOLE,
        .message = message,
    });
  }
  const auto *field = this->keyedField(type, index);
  if (field != nullptr) {
    return this->checkFieldDelete(index->object, field);
  }
  if (type->isObject()) {
    auto message = ErrorTextDelete::objectFld();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::OBJECT_FIELD,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextDelete::notField();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::NOT_A_FIELD,
        .message = message,
    });
  }

  return ok(unary);
}

const Field *UnaryChecker::keyedField(const Type *type, const IndexExpr *index) const {
  if (!type->isObject()) {
    return nullptr;
  }
  auto key = this->keyedFieldChecker->constantKey(index->index);
  if (!key.has_value()) {
    return nullptr;
  }

  return type->field(*key);
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkIn(BinaryExpr *binary) {
  auto key = this->simpleText(binary->lhs);
  bool isLiteralKey = isa<IntLitExpr>(binary->lhs);
  auto array = this->simpleText(binary->rhs).value_or(kUnnamedArray);
  binary->lhs = this->exprChecker->checkExpr(binary->lhs);
  binary->rhs = this->exprChecker->checkExpr(binary->rhs);
  binary->type = Type::boolTy();
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isError() || rightType->isError()) {
    return ok(binary);
  }
  const Type *right = rightType->nonOptional();
  if (right->isDict()) {
    return this->checkDictKeyIn(binary);
  }
  if (right->isUnion()) {
    return this->checkUnionIn(binary, rightType);
  }
  if (right->isObject()) {
    return this->checkObjectIn(binary, right);
  }
  if (!right->isArray()) {
    const char *op = Operators::opSpelling(BinaryOp::In);
    auto message = this->binaryChecker->operandTypesText(op, leftType, rightType);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool isKnownIndex = leftType->isInteger() && key.has_value();
  std::string indexText = isKnownIndex ? *key : kUnnamedIndex;
  auto check = this->inBoundsText(indexText, array, isLiteralKey);
  auto message = ErrorTextIn::array(check);

  return err(UnaryCheckerCheckInError{
      .code = UnaryCheckerCheckInErrorCode::ARRAY_IN,
      .message = message,
  });
}

std::string UnaryChecker::inBoundsText(const std::string &indexText, const std::string &array,
                                       bool isLiteralKey) const {
  auto below = indexText + " < " + array + ".length";
  if (isLiteralKey) {
    return below;
  }

  return indexText + " >= 0 && " + below;
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkDictKeyIn(BinaryExpr *binary) {
  auto has = this->dictChecker->checkDictIn(binary);
  if (has.isOk()) {
    return ok(has.value());
  }
  auto code = this->dictInCode(has.error().code);

  return err(UnaryCheckerCheckInError{
      .code = code,
      .message = has.error().message,
  });
}

UnaryCheckerCheckInErrorCode UnaryChecker::dictInCode(DictCheckerCheckDictInErrorCode code) const {
  switch (code) {
    case DictCheckerCheckDictInErrorCode::MAY_BE_UNDEFINED:
      return UnaryCheckerCheckInErrorCode::DICT_UNDEFINED;
    case DictCheckerCheckDictInErrorCode::NUMBER_KEY:
      return UnaryCheckerCheckInErrorCode::DICT_NUMBER_KEY;
    case DictCheckerCheckDictInErrorCode::NOT_A_RECORD:
      return UnaryCheckerCheckInErrorCode::DICT_NOT_RECORD;
  }

  return UnaryCheckerCheckInErrorCode::DICT_NOT_RECORD;
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkUnionIn(BinaryExpr *binary,
                                                                const Type *rightType) {
  if (rightType->isOptional()) {
    auto message = ErrorTextIn::undefined();

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::UNION_UNDEFINED,
        .message = message,
    });
  }
  const Type *options = rightType->nonOptional();
  auto *text = dyn_cast<StringLitExpr>(binary->lhs);
  auto name = text != nullptr ? this->keyedFieldChecker->constantKey(binary->lhs) : std::nullopt;
  const auto *objects = options->slotPart(UnionSlot::Object);
  if (!name.has_value() || objects == nullptr) {
    auto message = ErrorTextIn::byLiteral();

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::UNION_NO_KEY,
        .message = message,
    });
  }

  return this->checkUnionKeyIn(binary, options, objects, *name);
}

std::vector<const Type *> UnaryChecker::objectMembers(const Type *objects) const {
  if (!objects->isUnion()) {
    return {objects};
  }
  auto members = objects->members();

  return std::vector<const Type *>(members.begin(), members.end());
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkUnionKeyIn(BinaryExpr *binary,
                                                                   const Type *options,
                                                                   const Type *objects,
                                                                   const std::string &name) const {
  auto members = this->objectMembers(objects);
  for (const Type *member : members) {
    auto required = this->checkFieldRequired(binary, member, name);
    if (required.isErr()) {
      return err(required.error());
    }
  }
  size_t withField = this->countWithField(members, name);
  bool isAlwaysSame = withField == 0 || (withField == members.size() && objects == options);
  if (isAlwaysSame) {
    auto message = ErrorTextIn::always(withField != 0, options->name(), name);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::ALWAYS_SAME,
        .message = message,
    });
  }

  return ok(binary);
}

UnaryChecker::ResultInOperatorSuited
UnaryChecker::checkFieldRequired(BinaryExpr *binary, const Type *member,
                                 const std::string &name) const {
  const auto *field = member->field(name);
  if (field != nullptr && field->type->isOptional()) {
    auto shown = this->objectChecker->fieldText(binary->rhs, name);
    auto message = ErrorTextIn::maybeUndef(name, member->name(), shown);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OPTIONAL_FIELD,
        .message = message,
    });
  }

  return ok(binary);
}

size_t UnaryChecker::countWithField(llvm::ArrayRef<const Type *> members,
                                    const std::string &name) const {
  size_t count = 0;
  for (const Type *member : members) {
    if (member->field(name) != nullptr) {
      count += 1;
    }
  }

  return count;
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkObjectIn(BinaryExpr *binary,
                                                                 const Type *object) {
  if (object->isTuple()) {
    auto message = ErrorTextIn::tuple();

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::TUPLE_IN,
        .message = message,
    });
  }
  auto name = this->keyedFieldChecker->constantKey(binary->lhs);
  const Field *field = name.has_value() ? object->field(*name) : nullptr;
  if (field != nullptr && field->type->isOptional()) {
    auto text = this->objectChecker->fieldText(binary->rhs, *name);
    auto message = ErrorTextIn::objMaybe(text);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OBJECT_OPTIONAL,
        .message = message,
    });
  }
  if (field != nullptr) {
    auto message = ErrorTextIn::objTrue();

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_TRUE,
        .message = message,
    });
  }
  if (name.has_value()) {
    auto message = ErrorTextIn::objFalse(object->name(), *name);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_FALSE,
        .message = message,
    });
  }
  auto message = ErrorTextIn::objPlain();

  return err(UnaryCheckerCheckInError{
      .code = UnaryCheckerCheckInErrorCode::OBJECT_IN,
      .message = message,
  });
}

}
