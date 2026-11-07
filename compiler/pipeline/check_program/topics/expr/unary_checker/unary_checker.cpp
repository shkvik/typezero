#include "compiler/pipeline/check_program/topics/expr/unary_checker/unary_checker.h"
#include "compiler/pipeline/check_program/declare/constant_folder/constant_folder.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/topics/date/date_checker/date_checker.h"
#include "compiler/pipeline/check_program/topics/expr/binary_checker/binary_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/literal_checker/literal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/truth_checker/truth_checker.h"
#include "compiler/pipeline/check_program/topics/objects/dict_checker/dict_checker.h"
#include "compiler/pipeline/check_program/topics/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/pipeline/check_program/topics/objects/object_checker/object_checker.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

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
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return std::nullopt;
    }
    expr = conv->operand;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    std::optional<std::string> object = UnaryChecker::assertedText(member->object);
    if (!object) {
      return std::nullopt;
    }
    return *object + (member->optional ? "?." : ".") + member->name.str();
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    std::optional<std::string> object = UnaryChecker::assertedText(index->object);
    std::optional<std::string> position = UnaryChecker::assertedText(index->index);
    if (!object || !position) {
      return std::nullopt;
    }
    return *object + "[" + *position + "]";
  }
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->name.starts_with("%")) {
    return std::nullopt;
  }

  return UnaryChecker::simpleText(expr);
}

UnaryChecker::UnaryChecker(CheckState &owner)
    : state(owner) {}

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
  bool library = LibraryCalls(this->state).inLibrary(unary->loc());
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
  const char *op = Operators::opSpelling(unary->op);
  switch (unary->op) {
  case UnaryOp::Neg:
  case UnaryOp::Plus: {
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
      if (const Type *literal =
              LiteralTypes(this->state).numberLiteralFor(expected, -double(lit->value), false);
          literal && lit->value != 0) {
        LiteralChecker::ResultIntLiteralTyped typed =
            LiteralChecker(this->state).checkIntLit(lit, literal->literalBase(), true);
        if (typed.isErr()) {
          this->state.getDiagnostics().error(lit->loc(), typed.error().message);
        }
        unary->operand = typed.isOk() ? typed.value() : lit;
        unary->type = literal;
        return unary;
      }
    }
    if (auto *lit = dyn_cast<FloatLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
      if (const Type *literal =
              LiteralTypes(this->state).numberLiteralFor(expected, -lit->value, true)) {
        lit->type = Type::doubleTy();
        unary->type = literal;
        return unary;
      }
    }
    const Type *wanted = expected ? expected->nonOptional() : nullptr;
    if (const Type *member =
            LiteralTypes::numberMemberOf(expected, isa<FloatLitExpr>(unary->operand))) {
      wanted = member;
    }
    const Type *hint = wanted && (wanted->isNumeric() || wanted->isDecimal()) ? wanted : nullptr;
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand)) {
      LiteralChecker::ResultIntLiteralTyped typed =
          LiteralChecker(this->state).checkIntLit(lit, hint, unary->op == UnaryOp::Neg);
      if (typed.isErr()) {
        this->state.getDiagnostics().error(lit->loc(), typed.error().message);
      }
      unary->operand = typed.isOk() ? typed.value() : lit;
    } else {
      unary->operand =
          GenericChecker(this->state)
              .apparent(ExprChecker(this->state).checkExpr(unary->operand, hint, wraps));
    }
    const Type *type = unary->operand->type;
    if (type->isNever()) {
      unary->operand = TypeRelations(this->state).convert(unary->operand, Type::doubleTy());
      type = Type::doubleTy();
    }
    ResultNumberForTheSign sign = UnaryChecker::signOperand(op, type);
    if (sign.isErr()) {
      this->state.getDiagnostics().error(unary->loc(), sign.error().message);
      unary->type = Type::error();
      break;
    }
    if (type->isDate() && unary->op == UnaryOp::Plus) {
      return DateChecker(this->state).dateTime(unary->operand);
    }
    bool literal = isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand);
    ResultOperandIsNumeric numeric = UnaryChecker::numberOperand(unary, op, type, literal);
    if (numeric.isErr()) {
      this->state.getDiagnostics().error(unary->loc(), numeric.error().message);
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::Not: {
    Flow whenTrue, whenFalse;
    TruthChecker(this->state).checkBranching(unary, whenTrue, whenFalse);
    this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
    break;
  }
  case UnaryOp::BitNot: {
    const Type *hint =
        expected && expected->nonOptional()->isInteger() ? expected->nonOptional() : nullptr;
    unary->operand = GenericChecker(this->state)
                         .apparent(ExprChecker(this->state).checkExpr(unary->operand, hint));
    const Type *type = unary->operand->type;
    ResultOperandIsInteger integer = UnaryChecker::bitsOperand(type);
    if (integer.isErr()) {
      this->state.getDiagnostics().error(unary->loc(), integer.error().message);
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::TypeOf:
    unary->operand = ExprChecker(this->state).checkExpr(unary->operand);
    unary->type = Type::string();
    break;
  case UnaryOp::Void: {
    unary->operand = ExprChecker(this->state).checkExpr(unary->operand);
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
    break;
  }
  case UnaryOp::Delete: {
    ResultDeleteIsPossible deleted = this->checkDelete(unary);
    if (deleted.isErr()) {
      this->state.getDiagnostics().error(unary->loc(), deleted.error().message);
      return unary;
    }
    return deleted.value();
  }
  case UnaryOp::NonNull: {
    unary->operand = ExprChecker(this->state).checkExpr(unary->operand, expected);
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
    if (auto *name = dyn_cast<NameExpr>(unary->operand); name && name->var()) {
      FlowChecker(this->state).setDefined(name->var(), true);
    }
    return unary;
  }
  }
  if (unary->type && unary->type->isInteger()) {
    ConstantFolder(this->state).foldUnary(unary, wraps);
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
  if (auto *member = dyn_cast<MemberExpr>(unary->operand); member && !member->optional) {
    member->object = ExprChecker(this->state).checkExpr(member->object);
    const Type *object = member->object->type;
    if (object->isDict()) {
      DictChecker::ResultRecordKeyDeleted deleted =
          DictChecker(this->state).checkDictDelete(unary, member->object, nullptr, member->name);
      if (deleted.isErr()) {
        return err(UnaryCheckerCheckDeleteError{
            .code = UnaryCheckerCheckDeleteErrorCode::DICT_DELETE,
            .message = deleted.error().message,
        });
      }
      return ok(deleted.value());
    }
    if (object->isError()) {
      return ok(unary);
    }
    const Field *field =
        object->nonOptional()->isObject() ? object->nonOptional()->field(member->name) : nullptr;
    if (field) {
      return UnaryChecker::fieldDeleted(member->object, field);
    }
    std::string message = ErrorTextDelete::notField();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::NOT_A_FIELD,
        .message = message,
    });
  }
  if (!index) {
    unary->operand = ExprChecker(this->state).checkExpr(unary->operand);
    const Type *type = unary->operand->type;
    auto *member = dyn_cast<MemberExpr>(unary->operand);
    if (type->isError()) {
      return ok(unary);
    }
    const Type *object = member ? member->object->type->nonOptional() : nullptr;
    if (object && object->isObject() && object->field(member->name)) {
      return UnaryChecker::fieldDeleted(member->object, object->field(member->name));
    }
    std::string message = ErrorTextDelete::notField();

    return err(UnaryCheckerCheckDeleteError{
        .code = UnaryCheckerCheckDeleteErrorCode::NOT_A_FIELD,
        .message = message,
    });
  }
  std::string array = UnaryChecker::simpleText(index->object).value_or("a");
  std::string position = UnaryChecker::simpleText(index->index).value_or("i");
  index->object = ExprChecker(this->state).checkExpr(index->object);
  if (index->object->type->isDict()) {
    DictChecker::ResultRecordKeyDeleted deleted =
        DictChecker(this->state).checkDictDelete(unary, index->object, index->index, {});
    if (deleted.isErr()) {
      return err(UnaryCheckerCheckDeleteError{
          .code = UnaryCheckerCheckDeleteErrorCode::DICT_DELETE,
          .message = deleted.error().message,
      });
    }
    return ok(deleted.value());
  }
  index->index = ExprChecker(this->state).checkExpr(index->index);
  index->type = Type::error();
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
      type->isObject() ? KeyedFieldChecker(this->state).constantKey(index->index) : std::nullopt;
  if (key && type->field(*key)) {
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
  bin->lhs = ExprChecker(this->state).checkExpr(bin->lhs);
  bin->rhs = ExprChecker(this->state).checkExpr(bin->rhs);
  bin->type = Type::boolTy();
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isError() || r->isError()) {
    return ok(bin);
  }
  if (r->nonOptional()->isDict()) {
    DictChecker::ResultRecordKeyChecked has = DictChecker(this->state).checkDictIn(bin);
    if (has.isOk()) {
      return ok(has.value());
    }
    UnaryCheckerCheckInErrorCode code = UnaryCheckerCheckInErrorCode::DICT_NOT_RECORD;
    switch (has.error().code) {
      case DictCheckerCheckDictInErrorCode::MAY_BE_UNDEFINED:
        code = UnaryCheckerCheckInErrorCode::DICT_UNDEFINED;
        break;
      case DictCheckerCheckDictInErrorCode::NUMBER_KEY:
        code = UnaryCheckerCheckInErrorCode::DICT_NUMBER_KEY;
        break;
      case DictCheckerCheckDictInErrorCode::NOT_A_RECORD:
        code = UnaryCheckerCheckInErrorCode::DICT_NOT_RECORD;
        break;
    }

    return err(UnaryCheckerCheckInError{
        .code = code,
        .message = has.error().message,
    });
  }
  if (const Type *u = r->nonOptional(); u->isUnion()) {
    if (r->isOptional()) {
      std::string message = ErrorTextIn::undefined();

      return err(UnaryCheckerCheckInError{
          .code = UnaryCheckerCheckInErrorCode::UNION_UNDEFINED,
          .message = message,
      });
    }
    auto *text = dyn_cast<StringLitExpr>(bin->lhs);
    std::optional<std::string> name =
        text ? KeyedFieldChecker(this->state).constantKey(bin->lhs) : std::nullopt;
    const Type *objects = u->slotPart(UnionSlot::Object);
    if (!name || !objects) {
      std::string message = ErrorTextIn::byLiteral();

      return err(UnaryCheckerCheckInError{
          .code = UnaryCheckerCheckInErrorCode::UNION_NO_KEY,
          .message = message,
      });
    }
    const Type *holder = objects;
    size_t with = 0;
    size_t total = 0;
    for (const Type *m :
         objects->isUnion() ? objects->members() : llvm::ArrayRef<const Type *>(holder)) {
      ++total;
      const Field *f = m->field(*name);
      with += f != nullptr;
      if (f && f->type->isOptional()) {
        std::string field = ObjectChecker::fieldText(bin->rhs, *name);
        std::string message = ErrorTextIn::maybeUndef(*name, m->name(), field);

        return err(UnaryCheckerCheckInError{
            .code = UnaryCheckerCheckInErrorCode::OPTIONAL_FIELD,
            .message = message,
        });
      }
    }
    if (with == 0 || (with == total && objects == u)) {
      std::string message = ErrorTextIn::always(with != 0, u->name(), *name);

      return err(UnaryCheckerCheckInError{
          .code = UnaryCheckerCheckInErrorCode::ALWAYS_SAME,
          .message = message,
      });
    }
    return ok(bin);
  }
  if (const Type *object = r->nonOptional(); object->isObject()) {
    std::optional<std::string> name =
        object->isTuple() ? std::nullopt : KeyedFieldChecker(this->state).constantKey(bin->lhs);
    const Field *field = name ? object->field(*name) : nullptr;
    if (object->isTuple()) {
      std::string message = ErrorTextIn::tuple();

      return err(UnaryCheckerCheckInError{
          .code = UnaryCheckerCheckInErrorCode::TUPLE_IN,
          .message = message,
      });
    }
    if (field && field->type->isOptional()) {
      std::string text = ObjectChecker::fieldText(bin->rhs, *name);
      std::string message = ErrorTextIn::objMaybe(text);

      return err(UnaryCheckerCheckInError{
          .code = UnaryCheckerCheckInErrorCode::OBJECT_OPTIONAL,
          .message = message,
      });
    }
    if (field) {
      std::string message = ErrorTextIn::objTrue();

      return err(UnaryCheckerCheckInError{
          .code = UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_TRUE,
          .message = message,
      });
    }
    if (name) {
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
  if (!r->nonOptional()->isArray()) {
    std::string message = BinaryChecker::operandTypesText("in", l, r);

    return err(UnaryCheckerCheckInError{
        .code = UnaryCheckerCheckInErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  std::string i = l->isInteger() && key ? *key : "i";
  std::string check =
      literalKey ? i + " < " + array + ".length" : i + " >= 0 && " + i + " < " + array + ".length";
  std::string message = ErrorTextIn::array(check);

  return err(UnaryCheckerCheckInError{
      .code = UnaryCheckerCheckInErrorCode::ARRAY_IN,
      .message = message,
  });
}

}
