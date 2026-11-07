#include "compiler/check/expr/unary_checker/unary_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

static std::optional<std::string> simpleText(const Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    return std::to_string(lit->value);
  }

  return std::nullopt;
}

static std::optional<std::string> assertedText(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return std::nullopt;
    }
    expr = conv->operand;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    std::optional<std::string> object = assertedText(member->object);
    if (!object) {
      return std::nullopt;
    }
    return *object + (member->optional ? "?." : ".") + member->name.str();
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    std::optional<std::string> object = assertedText(index->object),
                               position = assertedText(index->index);
    if (!object || !position) {
      return std::nullopt;
    }
    return *object + "[" + *position + "]";
  }
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->name.starts_with("%")) {
    return std::nullopt;
  }

  return simpleText(expr);
}

UnaryChecker::UnaryChecker(Checker &owner)
    : checker(owner) {}

Expr *UnaryChecker::checkUnary(UnaryExpr *unary, const Type *expected, bool wraps) {
  const char *op = opSpelling(unary->op);
  switch (unary->op) {
  case UnaryOp::Neg:
  case UnaryOp::Plus: {
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
      if (const Type *literal =
              this->checker.literalTypes().numberLiteralFor(expected, -double(lit->value), false);
          literal && lit->value != 0) {
        LiteralChecker::ResultIntLiteralTyped typed =
            this->checker.literalChecker().checkIntLit(lit, literal->literalBase(), true);
        if (typed.isErr()) {
          this->checker.diags_.error(lit->loc(), typed.error().message);
        }
        unary->operand = typed.isOk() ? typed.value() : lit;
        unary->type = literal;
        return unary;
      }
    }
    if (auto *lit = dyn_cast<FloatLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
      if (const Type *literal =
              this->checker.literalTypes().numberLiteralFor(expected, -lit->value, true)) {
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
          this->checker.literalChecker().checkIntLit(lit, hint, unary->op == UnaryOp::Neg);
      if (typed.isErr()) {
        this->checker.diags_.error(lit->loc(), typed.error().message);
      }
      unary->operand = typed.isOk() ? typed.value() : lit;
    } else {
      unary->operand = this->checker.genericChecker().apparent(
          this->checker.exprChecker().checkExpr(unary->operand, hint, wraps));
    }
    const Type *type = unary->operand->type;
    if (type->isNever()) {
      unary->operand = this->checker.typeRelations().convert(unary->operand, Type::doubleTy());
      type = Type::doubleTy();
    }
    if (type->isString() || type->isBool()) {
      this->checker.diags_.error(
          unary->loc(),
          llvm::Twine("unary '") + op + "' does not turn a " +
              (type->isString() ? "string" : "boolean") + " into a number in tz; write " +
              (type->isString() ? "'Number(s)', which gives a Result" : "'Number(b)'"));
      unary->type = Type::error();
      break;
    }
    if (type->isDate() && unary->op == UnaryOp::Plus) {
      return this->checker.dateChecker().dateTime(unary->operand);
    }
    bool literal = isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand);
    if (type->isDecimal() && unary->op == UnaryOp::Plus && !literal) {
      this->checker.diags_.error(unary->loc(),
                                 "unary '+' cannot be applied to a decimal; for a double, write "
                                 "'Number(x)', or drop the '+'");
      type = Type::error();
    } else if (!type->isNumeric() && !type->isDecimal() && !type->isError()) {
      this->checker.diags_.error(unary->loc(), llvm::Twine("operator '") + op +
                                                   "' cannot be applied to type '" + type->name() +
                                                   "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::Not: {
    Checker::Flow whenTrue, whenFalse;
    this->checker.truthChecker().checkBranching(unary, whenTrue, whenFalse);
    this->checker.flow_ = FlowChecker::join(whenTrue, whenFalse);
    break;
  }
  case UnaryOp::BitNot: {
    const Type *hint =
        expected && expected->nonOptional()->isInteger() ? expected->nonOptional() : nullptr;
    unary->operand = this->checker.genericChecker().apparent(
        this->checker.exprChecker().checkExpr(unary->operand, hint));
    const Type *type = unary->operand->type;
    if (type->isFloat()) {
      this->checker.diags_.error(unary->loc(),
                                 "operator '~' works on the bits of integers, and this "
                                 "is a double; drop the fraction first, e.g. "
                                 "'Math.trunc(x).toInt()'");
      type = Type::error();
    } else if (!type->isInteger() && !type->isError()) {
      this->checker.diags_.error(unary->loc(),
                                 "operator '~' cannot be applied to type '" + type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::TypeOf:
    unary->operand = this->checker.exprChecker().checkExpr(unary->operand);
    unary->type = Type::string();
    break;
  case UnaryOp::Void: {
    unary->operand = this->checker.exprChecker().checkExpr(unary->operand);
    unary->type = Type::undefinedTy();
    const Type *dropped = unary->operand->type;
    bool promised = dropped->isPromise();
    if (promised) {
      dropped = dropped->promised();
    }
    if (Checker::holdsResults(dropped) && !this->checker.libraryCalls().inLibrary(unary->loc())) {
      this->checker.diags_.error(
          unary->loc(),
          promised ? std::string("'void' drops the Result that the Promise gives, so an error in "
                                 "it would go unchecked; let the task handle it, e.g. an async "
                                 "function that checks 'if (r.isErr()) ...'")
                   : std::string("'void' drops the Result, so an error in it would go unchecked; "
                                 "handle it, e.g. with '.match(...)'"));
    }
    break;
  }
  case UnaryOp::Delete:
    return this->checkDelete(unary);
  case UnaryOp::NonNull: {
    unary->operand = this->checker.exprChecker().checkExpr(unary->operand, expected);
    const Type *type = unary->operand->type;
    std::string text = assertedText(unary->operand).value_or("x");
    unary->type = type->isUndefined() ? Type::error() : type->nonOptional();
    if (type->isError()) {
      return unary;
    }
    if (type->isUndefined()) {
      this->checker.diags_.error(unary->loc(),
                                 "'!' is not supported, and the value is always undefined");
      return unary;
    }
    if (!type->isOptional()) {
      this->checker.diags_.error(unary->loc(), "'!' is not supported, and '" + text +
                                                   "' is never undefined here; drop the '!'");
      return unary;
    }
    this->checker.diags_.error(unary->loc(),
                               "'!' is not supported: check the value first, e.g. 'if (" + text +
                                   " == undefined) return ...', or give a "
                                   "default with '" +
                                   text + " ?? ...'");
    if (auto *name = dyn_cast<NameExpr>(unary->operand); name && name->var()) {
      this->checker.flowChecker().setDefined(name->var(), true);
    }
    return unary;
  }
  }
  if (unary->type && unary->type->isInteger()) {
    this->checker.constantFolder().foldUnary(unary, wraps);
  }

  return unary;
}

Expr *UnaryChecker::checkDelete(UnaryExpr *unary) {
  unary->type = Type::boolTy();
  auto reportField = [&](const Expr *object, const Field *field) {
    if (field->type->isOptional()) {
      this->checker.diags_.error(unary->loc(),
                                 "'delete' cannot remove field '" + field->name +
                                     "': a field that is not there is undefined; write '" +
                                     fieldText(object, field->name) + " = undefined'");
    } else {
      this->checker.diags_.error(unary->loc(),
                                 "'delete' cannot remove field '" + field->name +
                                     "': an object always has the fields of its type");
    }
  };
  auto *index = dyn_cast<IndexExpr>(unary->operand);
  if (auto *member = dyn_cast<MemberExpr>(unary->operand); member && !member->optional) {
    member->object = this->checker.exprChecker().checkExpr(member->object);
    const Type *object = member->object->type;
    if (object->isDict()) {
      return this->checker.dictChecker().checkDictDelete(unary, member->object, nullptr,
                                                         member->name);
    }
    if (object->isError()) {
      return unary;
    }
    if (object->nonOptional()->isObject() && object->nonOptional()->field(member->name)) {
      reportField(member->object, object->nonOptional()->field(member->name));
    } else {
      this->checker.diags_.error(unary->loc(),
                                 "'delete' removes fields of objects, and this is not a field");
    }
    return unary;
  }
  if (!index) {
    unary->operand = this->checker.exprChecker().checkExpr(unary->operand);
    const Type *type = unary->operand->type;
    auto *member = dyn_cast<MemberExpr>(unary->operand);
    if (type->isError()) {
      return unary;
    }
    const Type *object = member ? member->object->type->nonOptional() : nullptr;
    if (object && object->isObject() && object->field(member->name)) {
      reportField(member->object, object->field(member->name));
    } else {
      this->checker.diags_.error(unary->loc(),
                                 "'delete' removes fields of objects, and this is not a field");
    }
    return unary;
  }
  std::string array = simpleText(index->object).value_or("a");
  std::string position = simpleText(index->index).value_or("i");
  index->object = this->checker.exprChecker().checkExpr(index->object);
  if (index->object->type->isDict()) {
    return this->checker.dictChecker().checkDictDelete(unary, index->object, index->index, {});
  }
  index->index = this->checker.exprChecker().checkExpr(index->index);
  index->type = Type::error();
  const Type *type = index->object->type->nonOptional();
  if (type->isArray()) {
    this->checker.diags_.error(unary->loc(),
                               "'delete' would leave a hole in the array, and arrays have none; "
                               "remove the element with '" +
                                   array + ".splice(" + position + ", 1)', or replace it: '" +
                                   array + "[" + position + "] = ...'");
  } else if (type->isString()) {
    this->checker.diags_.error(unary->loc(),
                               "strings are immutable; a character cannot be deleted");
  } else if (type->isTuple()) {
    this->checker.diags_.error(
        unary->loc(), "'delete' would leave a hole in the tuple, which has a fixed length; assign "
                      "the element instead");
  } else if (std::optional<std::string> key =
                 type->isObject() ? this->checker.keyedFieldChecker().constantKey(index->index)
                                  : std::nullopt;
             key && type->field(*key)) {
    reportField(index->object, type->field(*key));
  } else if (type->isObject()) {
    this->checker.diags_.error(
        unary->loc(), "'delete' cannot remove a field: an object always has the fields of its "
                      "type");
  } else if (!type->isError()) {
    this->checker.diags_.error(unary->loc(),
                               "'delete' removes fields of objects, and this is not a field");
  }

  return unary;
}

Expr *UnaryChecker::checkIn(BinaryExpr *bin) {
  std::optional<std::string> key = simpleText(bin->lhs);
  bool literalKey = isa<IntLitExpr>(bin->lhs);
  std::string array = simpleText(bin->rhs).value_or("a");
  bin->lhs = this->checker.exprChecker().checkExpr(bin->lhs);
  bin->rhs = this->checker.exprChecker().checkExpr(bin->rhs);
  bin->type = Type::boolTy();
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isError() || r->isError()) {
    return bin;
  }
  if (r->nonOptional()->isDict()) {
    return this->checker.dictChecker().checkDictIn(bin);
  }
  if (const Type *u = r->nonOptional(); u->isUnion()) {
    if (r->isOptional()) {
      this->checker.diags_.error(bin->rhs->loc(),
                                 "the value may be undefined, and 'in' would throw then; check "
                                 "it first");
      return bin;
    }
    auto *text = dyn_cast<StringLitExpr>(bin->lhs);
    std::optional<std::string> name =
        text ? this->checker.keyedFieldChecker().constantKey(bin->lhs) : std::nullopt;
    const Type *objects = u->slotPart(UnionSlot::Object);
    if (!name || !objects) {
      this->checker.diags_.error(bin->loc(),
                                 "operator 'in' tells the objects of a union apart by a field, "
                                 "written as a string literal: '\"radius\" in shape'");
      return bin;
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
        this->checker.diags_.error(bin->loc(), "operator 'in' cannot test field '" + *name +
                                                   "' of '" + m->name() +
                                                   "', which may hold undefined, as one that is "
                                                   "not there; check the value instead: '" +
                                                   fieldText(bin->rhs, *name) + " != undefined'");
        return bin;
      }
    }
    if (with == 0 || (with == total && objects == u)) {
      this->checker.diags_.error(bin->loc(),
                                 "operator 'in' is always " + std::string(with ? "true" : "false") +
                                     " here: " + (with ? "every" : "no") + " member of '" +
                                     u->name() + "' has field '" + *name + "'");
    }
    return bin;
  }
  if (const Type *object = r->nonOptional(); object->isObject()) {
    std::optional<std::string> name =
        object->isTuple() ? std::nullopt : this->checker.keyedFieldChecker().constantKey(bin->lhs);
    const Field *field = name ? object->field(*name) : nullptr;
    if (object->isTuple()) {
      this->checker.diags_.error(
          bin->loc(),
          "operator 'in' is not supported for tuples: a tuple always has all its elements");
    } else if (field && field->type->isOptional()) {
      this->checker.diags_.error(
          bin->loc(), "operator 'in' is not supported for objects: a field that is not there is "
                      "undefined; check it with '" +
                          fieldText(bin->rhs, *name) + " != undefined'");
    } else if (field) {
      this->checker.diags_.error(
          bin->loc(), "operator 'in' is not supported for objects: an object always has the "
                      "fields of its type, so this would always be true");
    } else if (name) {
      this->checker.diags_.error(bin->loc(), "operator 'in' is not supported for objects: type '" +
                                                 object->name() + "' has no field '" + *name +
                                                 "', so this would always be false");
    } else {
      this->checker.diags_.error(
          bin->loc(), "operator 'in' is not supported for objects: an object always has the "
                      "fields of its type");
    }
    return bin;
  }
  if (!r->nonOptional()->isArray()) {
    this->checker.binaryChecker().reportOperandTypes(bin->loc(), "in", l, r);
    return bin;
  }
  std::string i = l->isInteger() && key ? *key : "i";
  std::string check =
      literalKey ? i + " < " + array + ".length" : i + " >= 0 && " + i + " < " + array + ".length";
  this->checker.diags_.error(
      bin->loc(), "operator 'in' is not supported for arrays, which have no holes; compare "
                  "the index with the length instead: '" +
                      check + "'");

  return bin;
}

}
