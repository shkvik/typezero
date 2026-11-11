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
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->literalChecker = &all.literalChecker;
  this->keyedFieldChecker = &all.keyedFieldChecker;
}

std::optional<std::string> UnaryChecker::simpleText(const Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    return std::to_string(lit->value);
  }

  return std::nullopt;
}

std::optional<std::string> UnaryChecker::assertedText(const Expr *expr) {
  std::vector<std::string> answers;
  for (const Expr *node : ChildrenFirst::order(expr, UnaryChecker::textParts)) {
    std::optional<std::string> text = UnaryChecker::nodeText(node, answers);
    if (!text.has_value()) {
      return std::nullopt;
    }
    answers.push_back(*text);
  }

  return answers.back();
}

NodeChildren<const Expr *> UnaryChecker::textParts(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    return {conv->operand};
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return {member->object};
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    return {index->object, index->index};
  }

  return {};
}

std::optional<std::string> UnaryChecker::nodeText(const Expr *expr,
                                                  std::vector<std::string> &answers) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && !conv->isImplicit()) {
    return std::nullopt;
  }
  if (isa<CastExpr>(expr)) {
    return ChildrenFirst::takeAnswer(answers);
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    std::string object = ChildrenFirst::takeAnswer(answers);
    return object + (member->optional ? "?." : ".") + member->name.str();
  }
  if (isa<IndexExpr>(expr)) {
    std::string position = ChildrenFirst::takeAnswer(answers);
    std::string object = ChildrenFirst::takeAnswer(answers);
    return object + "[" + position + "]";
  }
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->name.starts_with("%")) {
    return std::nullopt;
  }

  return UnaryChecker::simpleText(expr);
}

UnaryChecker::ResultNumberForTheSign UnaryChecker::signOperand(llvm::StringRef op,
                                                               const Type *type) {
  if (type->isString() || type->isBool()) {
    std::string message = ErrorTextUnary::noNumber(op, type->isString());

    return err(UnaryCheckerSignOperandError{
        .code = UnaryCheckerSignOperandErrorCode::STRING_OR_BOOL,
        .message = message,
    });
  }

  return ok(type);
}

UnaryChecker::ResultOperandIsNumeric UnaryChecker::numberOperand(const UnaryExpr *unary,
                                                                 llvm::StringRef op,
                                                                 const Type *type, bool literal) {
  if (type->isDecimal() && unary->op == UnaryOp::Plus && !literal) {
    std::string message = ErrorTextUnary::decimalSum();

    return err(UnaryCheckerNumberOperandError{
        .code = UnaryCheckerNumberOperandErrorCode::DECIMAL_PLUS,
        .message = message,
    });
  }
  if (!type->isNumeric() && !type->isDecimal() && !type->isError()) {
    std::string message = ErrorTextUnary::notNumber(op, type->name());

    return err(UnaryCheckerNumberOperandError{
        .code = UnaryCheckerNumberOperandErrorCode::NOT_NUMERIC,
        .message = message,
    });
  }

  return ok(type);
}

UnaryChecker::ResultOperandIsInteger UnaryChecker::bitsOperand(const Type *type) {
  if (type->isFloat()) {
    std::string message = ErrorTextUnary::bitsDouble();

    return err(UnaryCheckerBitsOperandError{
        .code = UnaryCheckerBitsOperandErrorCode::DOUBLE_BITS,
        .message = message,
    });
  }
  if (!type->isInteger() && !type->isError()) {
    std::string message = ErrorTextUnary::bitsType(type->name());

    return err(UnaryCheckerBitsOperandError{
        .code = UnaryCheckerBitsOperandErrorCode::NOT_INTEGER,
        .message = message,
    });
  }

  return ok(type);
}

UnaryChecker::ResultResultNotDropped
UnaryChecker::resultKept(const UnaryExpr *unary, const Type *dropped, bool promised) const {
  bool library = this->libraryCalls->inLibrary(unary->loc());
  if (dropped->holdsResults() && !library) {
    std::string message = promised ? ErrorTextUnary::dropsAsync() : ErrorTextUnary::drops();

    return err(UnaryCheckerResultKeptError{
        .code = UnaryCheckerResultKeptErrorCode::DROPS_RESULT,
        .message = message,
    });
  }

  return ok(dropped);
}

UnaryChecker::ResultNonNullAssertion UnaryChecker::nonNullOperand(const Type *type,
                                                                  llvm::StringRef text) {
  if (type->isUndefined()) {
    std::string message = ErrorTextUnary::allUndef();

    return err(UnaryCheckerNonNullOperandError{
        .code = UnaryCheckerNonNullOperandErrorCode::ALWAYS_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isOptional()) {
    std::string message = ErrorTextUnary::neverUndef(text);

    return err(UnaryCheckerNonNullOperandError{
        .code = UnaryCheckerNonNullOperandErrorCode::NEVER_UNDEFINED,
        .message = message,
    });
  }
  std::string message = ErrorTextUnary::checkFirst(text);

  return err(UnaryCheckerNonNullOperandError{
      .code = UnaryCheckerNonNullOperandErrorCode::NOT_SUPPORTED,
      .message = message,
  });
}

Expr *UnaryChecker::checkUnary(UnaryExpr *unary, const Type *expected, bool wraps) {
  switch (unary->op) {
    case UnaryOp::Neg:
    case UnaryOp::Plus:
      return this->checkSign(unary, expected, wraps);
    case UnaryOp::Not:
      return this->checkNot(unary, wraps);
    case UnaryOp::BitNot:
      return this->checkBitNot(unary, expected, wraps);
    case UnaryOp::TypeOf:
      unary->operand = this->exprChecker->checkExpr(unary->operand);
      unary->type = Type::string();
      return this->foldedUnary(unary, wraps);
    case UnaryOp::Void:
      return this->checkVoid(unary, wraps);
    case UnaryOp::Delete:
      return this->checkDeleteValue(unary);
    case UnaryOp::NonNull:
      return this->checkNonNull(unary, expected);
  }

  return unary;
}

Expr *UnaryChecker::foldedUnary(UnaryExpr *unary, bool wraps) {
  if (unary->type != nullptr && unary->type->isInteger()) {
    this->constantFolder->foldUnary(unary, wraps);
  }

  return unary;
}

Expr *UnaryChecker::checkSign(UnaryExpr *unary, const Type *expected, bool wraps) {
  if (unary->op == UnaryOp::Neg) {
    Expr *negative = this->checkNegativeLiteral(unary, expected);
    if (negative != nullptr) {
      return negative;
    }
  }
  const Type *hint = UnaryChecker::signHint(unary, expected);
  auto *lit = dyn_cast<IntLitExpr>(unary->operand);
  if (lit != nullptr) {
    unary->operand = this->checkIntLiteral(lit, hint, unary->op == UnaryOp::Neg);
  } else {
    Expr *checked = this->exprChecker->checkExpr(unary->operand, hint, wraps);
    unary->operand = this->genericChecker->apparent(checked);
  }
  const Type *type = unary->operand->type;
  if (type->isNever()) {
    unary->operand = this->typeRelations->convert(unary->operand, Type::doubleTy());
    type = Type::doubleTy();
  }
  const char *op = Operators::opSpelling(unary->op);
  ResultNumberForTheSign sign = UnaryChecker::signOperand(op, type);
  if (sign.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), sign.error().message);
    unary->type = Type::error();
    return this->foldedUnary(unary, wraps);
  }
  if (type->isDate() && unary->op == UnaryOp::Plus) {
    return this->dateChecker->dateTime(unary->operand);
  }
  bool literal = isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand);
  ResultOperandIsNumeric numeric = UnaryChecker::numberOperand(unary, op, type, literal);
  if (numeric.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), numeric.error().message);
    type = Type::error();
  }
  unary->type = type;

  return this->foldedUnary(unary, wraps);
}

Expr *UnaryChecker::checkNegativeLiteral(UnaryExpr *unary, const Type *expected) {
  auto *integer = dyn_cast<IntLitExpr>(unary->operand);
  if (integer != nullptr) {
    const Type *literal =
        this->literalTypes->numberLiteralFor(expected, -double(integer->value), false);
    if (literal == nullptr || integer->value == 0) {
      return nullptr;
    }
    unary->operand = this->checkIntLiteral(integer, literal->literalBase(), true);
    unary->type = literal;
    return unary;
  }
  auto *real = dyn_cast<FloatLitExpr>(unary->operand);
  if (real == nullptr) {
    return nullptr;
  }
  const Type *literal = this->literalTypes->numberLiteralFor(expected, -real->value, true);
  if (literal == nullptr) {
    return nullptr;
  }
  real->type = Type::doubleTy();
  unary->type = literal;

  return unary;
}

Expr *UnaryChecker::checkIntLiteral(IntLitExpr *lit, const Type *hint, bool negative) {
  LiteralChecker::ResultIntLiteralTyped typed =
      this->literalChecker->checkIntLit(lit, hint, negative);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), typed.error().message);
    return lit;
  }

  return typed.value();
}

const Type *UnaryChecker::signHint(const UnaryExpr *unary, const Type *expected) {
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *member = LiteralTypes::numberMemberOf(expected, isa<FloatLitExpr>(unary->operand));
  if (member != nullptr) {
    wanted = member;
  }
  bool numeric = wanted != nullptr && (wanted->isNumeric() || wanted->isDecimal());

  return numeric ? wanted : nullptr;
}

Expr *UnaryChecker::checkNot(UnaryExpr *unary, bool wraps) {
  Flow whenTrue;
  Flow whenFalse;
  this->truthChecker->checkBranching(unary, whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));

  return this->foldedUnary(unary, wraps);
}

Expr *UnaryChecker::checkBitNot(UnaryExpr *unary, const Type *expected, bool wraps) {
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *hint = wanted != nullptr && wanted->isInteger() ? wanted : nullptr;
  Expr *checked = this->exprChecker->checkExpr(unary->operand, hint);
  unary->operand = this->genericChecker->apparent(checked);
  const Type *type = unary->operand->type;
  ResultOperandIsInteger integer = UnaryChecker::bitsOperand(type);
  if (integer.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), integer.error().message);
    type = Type::error();
  }
  unary->type = type;

  return this->foldedUnary(unary, wraps);
}

Expr *UnaryChecker::checkVoid(UnaryExpr *unary, bool wraps) {
  unary->operand = this->exprChecker->checkExpr(unary->operand);
  unary->type = Type::undefinedTy();
  const Type *dropped = unary->operand->type;
  bool promised = dropped->isPromise();
  if (promised) {
    dropped = dropped->promised();
  }
  ResultResultNotDropped kept = this->resultKept(unary, dropped, promised);
  if (kept.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), kept.error().message);
  }

  return this->foldedUnary(unary, wraps);
}

Expr *UnaryChecker::checkDeleteValue(UnaryExpr *unary) {
  ResultDeleteIsPossible deleted = this->checkDelete(unary);
  if (deleted.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), deleted.error().message);
    return unary;
  }

  return deleted.value();
}

Expr *UnaryChecker::checkNonNull(UnaryExpr *unary, const Type *expected) {
  unary->operand = this->exprChecker->checkExpr(unary->operand, expected);
  const Type *type = unary->operand->type;
  std::string text = UnaryChecker::assertedText(unary->operand).value_or("x");
  unary->type = type->isUndefined() ? Type::error() : type->nonOptional();
  if (type->isError()) {
    return unary;
  }
  ResultNonNullAssertion asserted = UnaryChecker::nonNullOperand(type, text);
  if (asserted.isErr()) {
    this->state.getDiagnostics().error(unary->loc(), asserted.error().message);
  }
  if (asserted.isErr() &&
      asserted.error().code != UnaryCheckerNonNullOperandErrorCode::NOT_SUPPORTED) {
    return unary;
  }
  auto *name = dyn_cast<NameExpr>(unary->operand);
  if (name != nullptr && name->var() != nullptr) {
    this->flowChecker->setDefined(name->var(), true);
  }

  return unary;
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::fieldDeleted(const Expr *object,
                                                                const Field *field) {
  if (field->type->isOptional()) {
    std::string text = ObjectChecker::fieldText(object, field->name);
    std::string message = ErrorTextDelete::optional(field->name, text);

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::OPTIONAL_FIELD,
        .message = message,
    });
  }
  std::string message = ErrorTextDelete::required(field->name);

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
UnaryChecker::dictDeleted(Result<Expr *, DictCheckerCheckDictDeleteError> deleted) {
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
    DictChecker::ResultRecordKeyDeleted deleted =
        this->dictChecker->checkDictDelete(unary, member->object, nullptr, member->name);
    return UnaryChecker::dictDeleted(deleted);
  }
  if (object->isError()) {
    return ok(unary);
  }
  const Type *own = object->nonOptional();
  const Field *field = own->isObject() ? own->field(member->name) : nullptr;
  if (field != nullptr) {
    return UnaryChecker::fieldDeleted(member->object, field);
  }
  std::string message = ErrorTextDelete::notField();

  return err(UnaryCheckerCheckDeleteError{
      .code = UnaryCheckerCheckDeleteErrorCode::NOT_A_FIELD,
      .message = message,
  });
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::checkOtherDelete(UnaryExpr *unary) {
  unary->operand = this->exprChecker->checkExpr(unary->operand);
  const Type *type = unary->operand->type;
  auto *member = dyn_cast<MemberExpr>(unary->operand);
  if (type->isError()) {
    return ok(unary);
  }
  const Type *object = member != nullptr ? member->object->type->nonOptional() : nullptr;
  if (object != nullptr && object->isObject() && object->field(member->name) != nullptr) {
    return UnaryChecker::fieldDeleted(member->object, object->field(member->name));
  }
  std::string message = ErrorTextDelete::notField();

  return err(UnaryCheckerCheckDeleteError{
      .code = UnaryCheckerCheckDeleteErrorCode::NOT_A_FIELD,
      .message = message,
  });
}

UnaryChecker::ResultDeleteIsPossible UnaryChecker::checkIndexDelete(UnaryExpr *unary,
                                                                    IndexExpr *index) {
  std::string array = UnaryChecker::simpleText(index->object).value_or("a");
  std::string position = UnaryChecker::simpleText(index->index).value_or("i");
  index->object = this->exprChecker->checkExpr(index->object);
  if (index->object->type->isDict()) {
    DictChecker::ResultRecordKeyDeleted deleted =
        this->dictChecker->checkDictDelete(unary, index->object, index->index, {});
    return UnaryChecker::dictDeleted(deleted);
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
    std::string message = ErrorTextDelete::hole(array, position);

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::ARRAY_HOLE,
        .message = message,
    });
  }
  if (type->isString()) {
    std::string message = ErrorTextDelete::character();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::STRING_CHARACTER,
        .message = message,
    });
  }
  if (type->isTuple()) {
    std::string message = ErrorTextDelete::tupleHole();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::TUPLE_HOLE,
        .message = message,
    });
  }
  std::optional<std::string> key =
      type->isObject() ? this->keyedFieldChecker->constantKey(index->index) : std::nullopt;
  if (key.has_value() && type->field(*key) != nullptr) {
    return UnaryChecker::fieldDeleted(index->object, type->field(*key));
  }
  if (type->isObject()) {
    std::string message = ErrorTextDelete::objectFld();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::OBJECT_FIELD,
        .message = message,
    });
  }
  if (!type->isError()) {
    std::string message = ErrorTextDelete::notField();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::NOT_A_FIELD,
        .message = message,
    });
  }

  return ok(unary);
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkIn(BinaryExpr *bin) {
  std::optional<std::string> key = UnaryChecker::simpleText(bin->lhs);
  bool literalKey = isa<IntLitExpr>(bin->lhs);
  std::string array = UnaryChecker::simpleText(bin->rhs).value_or("a");
  bin->lhs = this->exprChecker->checkExpr(bin->lhs);
  bin->rhs = this->exprChecker->checkExpr(bin->rhs);
  bin->type = Type::boolTy();
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isError() || r->isError()) {
    return ok(bin);
  }
  const Type *right = r->nonOptional();
  if (right->isDict()) {
    return this->checkDictKeyIn(bin);
  }
  if (right->isUnion()) {
    return this->checkUnionIn(bin, r);
  }
  if (right->isObject()) {
    return this->checkObjectIn(bin, right);
  }
  if (!right->isArray()) {
    std::string message = BinaryChecker::operandTypesText("in", l, r);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  std::string i = l->isInteger() && key.has_value() ? *key : "i";
  std::string check =
      literalKey ? i + " < " + array + ".length" : i + " >= 0 && " + i + " < " + array + ".length";
  std::string message = ErrorTextIn::array(check);

  return err(UnaryCheckerCheckInError{
      .code = UnaryCheckerCheckInErrorCode::ARRAY_IN,
      .message = message,
  });
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkDictKeyIn(BinaryExpr *bin) {
  DictChecker::ResultRecordKeyChecked has = this->dictChecker->checkDictIn(bin);
  if (has.isOk()) {
    return ok(has.value());
  }
  UnaryCheckerCheckInErrorCode code = UnaryChecker::dictInCode(has.error().code);

  return err(UnaryCheckerCheckInError{
      .code = code,
      .message = has.error().message,
  });
}

UnaryCheckerCheckInErrorCode UnaryChecker::dictInCode(DictCheckerCheckDictInErrorCode code) {
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

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkUnionIn(BinaryExpr *bin, const Type *r) {
  if (r->isOptional()) {
    std::string message = ErrorTextIn::undefined();

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::UNION_UNDEFINED,
        .message = message,
    });
  }
  const Type *u = r->nonOptional();
  auto *text = dyn_cast<StringLitExpr>(bin->lhs);
  std::optional<std::string> name =
      text != nullptr ? this->keyedFieldChecker->constantKey(bin->lhs) : std::nullopt;
  const Type *objects = u->slotPart(UnionSlot::Object);
  if (!name.has_value() || objects == nullptr) {
    std::string message = ErrorTextIn::byLiteral();

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::UNION_NO_KEY,
        .message = message,
    });
  }

  return UnaryChecker::unionKeyIn(bin, u, objects, *name);
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::unionKeyIn(BinaryExpr *bin, const Type *u,
                                                              const Type *objects,
                                                              const std::string &name) {
  const Type *holder = objects;
  llvm::ArrayRef<const Type *> members =
      objects->isUnion() ? objects->members() : llvm::ArrayRef<const Type *>(holder);
  size_t with = 0;
  for (const Type *m : members) {
    const Field *f = m->field(name);
    with += f != nullptr;
    if (f != nullptr && f->type->isOptional()) {
      std::string field = ObjectChecker::fieldText(bin->rhs, name);
      std::string message = ErrorTextIn::maybeUndef(name, m->name(), field);

      return err(UnaryCheckerCheckInError{
          .code = UnaryCheckerCheckInErrorCode::OPTIONAL_FIELD,
          .message = message,
      });
    }
  }
  if (with == 0 || (with == members.size() && objects == u)) {
    std::string message = ErrorTextIn::always(with != 0, u->name(), name);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::ALWAYS_SAME,
        .message = message,
    });
  }

  return ok(bin);
}

UnaryChecker::ResultInOperatorSuited UnaryChecker::checkObjectIn(BinaryExpr *bin,
                                                                 const Type *object) {
  if (object->isTuple()) {
    std::string message = ErrorTextIn::tuple();

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::TUPLE_IN,
        .message = message,
    });
  }
  std::optional<std::string> name = this->keyedFieldChecker->constantKey(bin->lhs);
  const Field *field = name.has_value() ? object->field(*name) : nullptr;
  if (field != nullptr && field->type->isOptional()) {
    std::string text = ObjectChecker::fieldText(bin->rhs, *name);
    std::string message = ErrorTextIn::objMaybe(text);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OBJECT_OPTIONAL,
        .message = message,
    });
  }
  if (field != nullptr) {
    std::string message = ErrorTextIn::objTrue();

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_TRUE,
        .message = message,
    });
  }
  if (name.has_value()) {
    std::string message = ErrorTextIn::objFalse(object->name(), *name);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_FALSE,
        .message = message,
    });
  }
  std::string message = ErrorTextIn::objPlain();

  return err(UnaryCheckerCheckInError{
      .code = UnaryCheckerCheckInErrorCode::OBJECT_IN,
      .message = message,
  });
}

}
