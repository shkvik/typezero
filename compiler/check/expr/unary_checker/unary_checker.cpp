#include "compiler/check/expr/unary_checker/unary_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

// A name or an integer literal as written, for messages that show how to
// rewrite the code around it.
static std::optional<std::string> simpleText(const Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    return std::to_string(lit->value);
  }

  return std::nullopt;
}

UnaryChecker::UnaryChecker(Checker &checker) : checker_(checker) {}

Expr *UnaryChecker::checkUnary(UnaryExpr *unary, const Type *expected, bool wraps) {
  const char *op = opSpelling(unary->op);
  switch (unary->op) {
  case UnaryOp::Neg:
  case UnaryOp::Plus: {
    // `-1` where a literal type is expected, `-1 | 1`: the literal is
    // checked as an int and negated, and the result is of the literal type.
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
      if (const Type *literal =
              checker_.literalTypes().numberLiteralFor(expected, -double(lit->value), false);
          literal && lit->value != 0) {
        unary->operand =
            checker_.literalChecker().checkIntLit(lit, literal->literalBase(), /*negated=*/true);
        unary->type = literal;
        return unary;
      }
    }
    if (auto *lit = dyn_cast<FloatLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
      if (const Type *literal =
              checker_.literalTypes().numberLiteralFor(expected, -lit->value, true)) {
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
      unary->operand =
          checker_.literalChecker().checkIntLit(lit, hint, /*negated=*/unary->op == UnaryOp::Neg);
    } else {
      unary->operand = checker_.genericChecker().apparent(
          checker_.exprChecker().checkExpr(unary->operand, hint, wraps));
    }
    const Type *type = unary->operand->type;
    if (type->isNever()) { // see neverOperands
      unary->operand = checker_.typeRelations().convert(unary->operand, Type::doubleTy());
      type = Type::doubleTy();
    }
    if (type->isString() || type->isBool()) {
      // `+s` is Number(s), and `-s` is -Number(s), as in JavaScript.
      unary->type = Type::doubleTy();
      break;
    }
    // `+d` is the time of a date.
    if (type->isDate() && unary->op == UnaryOp::Plus) {
      return checker_.dateChecker().dateTime(unary->operand);
    }
    bool literal = isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand);
    if (type->isDecimal() && unary->op == UnaryOp::Plus && !literal) {
      // `+x` makes a number of x in JavaScript, which a decimal does only
      // explicitly; `+2` is just 2.
      checker_.diags_.error(unary->loc(),
                            "unary '+' cannot be applied to a decimal; for a double, write "
                            "'Number(x)', or drop the '+'");
      type = Type::error();
    } else if (!type->isNumeric() && !type->isDecimal() && !type->isError()) {
      checker_.diags_.error(unary->loc(), llvm::Twine("operator '") + op +
                                              "' cannot be applied to type '" + type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::Not: {
    Checker::Flow whenTrue, whenFalse;
    checker_.truthChecker().checkBranching(unary, whenTrue, whenFalse);
    checker_.flow_ = FlowChecker::join(whenTrue, whenFalse);
    break;
  }
  case UnaryOp::BitNot: {
    const Type *hint =
        expected && expected->nonOptional()->isInteger() ? expected->nonOptional() : nullptr;
    unary->operand = checker_.genericChecker().apparent(
        checker_.exprChecker().checkExpr(unary->operand, hint, /*wraps=*/true));
    const Type *type = unary->operand->type;
    if (type->isFloat()) {
      unary->operand = checker_.binaryChecker().toInt32(unary->operand);
      type = Type::intTy();
    } else if (!type->isInteger() && !type->isError()) {
      checker_.diags_.error(unary->loc(),
                            "operator '~' cannot be applied to type '" + type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::TypeOf:
    // Known from the type, except whether a `T | undefined` holds a value.
    unary->operand = checker_.exprChecker().checkExpr(unary->operand);
    unary->type = Type::string();
    break;
  case UnaryOp::Void:
    // `void f()` runs f and drops what it returns; the value is undefined,
    // usable wherever the literal `undefined` is.
    unary->operand = checker_.exprChecker().checkExpr(unary->operand);
    unary->type = Type::undefinedTy();
    break;
  case UnaryOp::Delete:
    return checkDelete(unary);
  case UnaryOp::NonNull: {
    // `x!`: the program stops if x is undefined, so from here on a variable
    // holds a value.
    unary->operand = checker_.exprChecker().checkExpr(unary->operand, expected);
    const Type *type = unary->operand->type;
    if (type->isUndefined()) {
      checker_.diags_.error(unary->loc(), "the value is always undefined");
      type = Type::error();
    }
    unary->type = type->nonOptional();
    if (auto *name = dyn_cast<NameExpr>(unary->operand); name && name->var && type->isOptional()) {
      checker_.flowChecker().setDefined(name->var, true);
    } else if (std::optional<Checker::FieldPath> path =
                   checker_.fieldFacts().fieldPath(unary->operand);
               path && type->isOptional()) {
      FieldFacts::defineField(checker_.flow_, *path);
    }
    break;
  }
  }
  if (unary->type && unary->type->isInteger()) {
    checker_.constantFolder().foldUnary(unary, wraps);
  }

  return unary;
}

// `delete` removes a property of an object, and an object always has the
// fields of its type. An array element would leave a hole, and arrays have
// none.
Expr *UnaryChecker::checkDelete(UnaryExpr *unary) {
  unary->type = Type::boolTy();
  // A field that is not there and one that holds undefined are the same.
  auto reportField = [&](const Expr *object, const Field *field) {
    if (field->type->isOptional()) {
      checker_.diags_.error(unary->loc(), "'delete' cannot remove field '" + field->name +
                                              "': a field that is not there is undefined; write '" +
                                              fieldText(object, field->name) + " = undefined'");
    } else {
      checker_.diags_.error(unary->loc(), "'delete' cannot remove field '" + field->name +
                                              "': an object always has the fields of its type");
    }
  };
  auto *index = dyn_cast<IndexExpr>(unary->operand);
  if (auto *member = dyn_cast<MemberExpr>(unary->operand); member && !member->optional) {
    // The object first: `delete r.key` removes a key of a Record.
    member->object = checker_.exprChecker().checkExpr(member->object);
    const Type *object = member->object->type;
    if (object->isDict()) {
      return checker_.dictChecker().checkDictDelete(unary, member->object, nullptr, member->name);
    }
    if (object->isError()) {
      return unary;
    }
    if (object->nonOptional()->isObject() && object->nonOptional()->field(member->name)) {
      reportField(member->object, object->nonOptional()->field(member->name));
    } else {
      checker_.diags_.error(unary->loc(),
                            "'delete' removes fields of objects, and this is not a field");
    }
    return unary;
  }
  if (!index) {
    unary->operand = checker_.exprChecker().checkExpr(unary->operand);
    const Type *type = unary->operand->type;
    auto *member = dyn_cast<MemberExpr>(unary->operand);
    if (type->isError()) {
      return unary;
    }
    const Type *object = member ? member->object->type->nonOptional() : nullptr;
    if (object && object->isObject() && object->field(member->name)) {
      reportField(member->object, object->field(member->name));
    } else {
      checker_.diags_.error(unary->loc(),
                            "'delete' removes fields of objects, and this is not a field");
    }
    return unary;
  }
  std::string array = simpleText(index->object).value_or("a");
  std::string position = simpleText(index->index).value_or("i");
  index->object = checker_.exprChecker().checkExpr(index->object);
  if (index->object->type->isDict()) {
    return checker_.dictChecker().checkDictDelete(unary, index->object, index->index, {});
  }
  index->index = checker_.exprChecker().checkExpr(index->index);
  index->type = Type::error();
  const Type *type = index->object->type->nonOptional();
  if (type->isArray()) {
    checker_.diags_.error(unary->loc(),
                          "'delete' would leave a hole in the array, and arrays have none; "
                          "remove the element with '" +
                              array + ".splice(" + position + ", 1)', or replace it: '" + array +
                              "[" + position + "] = ...'");
  } else if (type->isString()) {
    checker_.diags_.error(unary->loc(), "strings are immutable; a character cannot be deleted");
  } else if (type->isTuple()) {
    checker_.diags_.error(
        unary->loc(), "'delete' would leave a hole in the tuple, which has a fixed length; assign "
                      "the element instead");
  } else if (std::optional<std::string> key =
                 type->isObject() ? checker_.keyedFieldChecker().constantKey(index->index)
                                  : std::nullopt;
             key && type->field(*key)) {
    reportField(index->object, type->field(*key));
  } else if (type->isObject()) {
    checker_.diags_.error(unary->loc(),
                          "'delete' cannot remove a field: an object always has the fields of its "
                          "type");
  } else if (!type->isError()) {
    checker_.diags_.error(unary->loc(),
                          "'delete' removes fields of objects, and this is not a field");
  }

  return unary;
}

// `key in object` asks whether the object has the property; an object always
// has the fields of its type. Arrays have no holes, so for them it is a
// comparison of the index with the length.
Expr *UnaryChecker::checkIn(BinaryExpr *bin) {
  std::optional<std::string> key = simpleText(bin->lhs);
  bool literalKey = isa<IntLitExpr>(bin->lhs);
  std::string array = simpleText(bin->rhs).value_or("a");
  bin->lhs = checker_.exprChecker().checkExpr(bin->lhs);
  bin->rhs = checker_.exprChecker().checkExpr(bin->rhs);
  bin->type = Type::boolTy();
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  if (l->isError() || r->isError()) {
    return bin;
  }
  if (r->nonOptional()->isDict()) {
    return checker_.dictChecker().checkDictIn(bin);
  }
  // `"radius" in s` tells the members of a union of objects apart by their
  // fields (docs/objects.md, section 7).
  if (const Type *u = r->nonOptional(); u->isUnion()) {
    if (r->isOptional()) {
      checker_.diags_.error(bin->rhs->loc(),
                            "the value may be undefined, and 'in' would throw then; check "
                            "it first");
      return bin;
    }
    auto *text = dyn_cast<StringLitExpr>(bin->lhs);
    std::optional<std::string> name =
        text ? checker_.keyedFieldChecker().constantKey(bin->lhs) : std::nullopt;
    const Type *objects = u->slotPart(UnionSlot::Object);
    if (!name || !objects) {
      checker_.diags_.error(bin->loc(),
                            "operator 'in' tells the objects of a union apart by a field, "
                            "written as a string literal: '\"radius\" in shape'");
      return bin;
    }
    const Type *holder = objects;
    size_t with = 0, total = 0;
    for (const Type *m :
         objects->isUnion() ? objects->members() : llvm::ArrayRef<const Type *>(holder)) {
      ++total;
      const Field *f = m->field(*name);
      with += f != nullptr;
      if (f && f->type->isOptional()) {
        checker_.diags_.error(bin->loc(), "operator 'in' cannot test field '" + *name + "' of '" +
                                              m->name() +
                                              "', which may hold undefined, as one that is "
                                              "not there; check the value instead: '" +
                                              fieldText(bin->rhs, *name) + " != undefined'");
        return bin;
      }
    }
    if (with == 0 || (with == total && objects == u)) {
      checker_.diags_.error(bin->loc(), "operator 'in' is always " +
                                            std::string(with ? "true" : "false") +
                                            " here: " + (with ? "every" : "no") + " member of '" +
                                            u->name() + "' has field '" + *name + "'");
    }
    return bin;
  }
  if (const Type *object = r->nonOptional(); object->isObject()) {
    // A field that is not there and one that holds undefined are the same.
    std::optional<std::string> name =
        object->isTuple() ? std::nullopt : checker_.keyedFieldChecker().constantKey(bin->lhs);
    const Field *field = name ? object->field(*name) : nullptr;
    if (object->isTuple()) {
      checker_.diags_.error(
          bin->loc(),
          "operator 'in' is not supported for tuples: a tuple always has all its elements");
    } else if (field && field->type->isOptional()) {
      checker_.diags_.error(
          bin->loc(), "operator 'in' is not supported for objects: a field that is not there is "
                      "undefined; check it with '" +
                          fieldText(bin->rhs, *name) + " != undefined'");
    } else if (field) {
      checker_.diags_.error(bin->loc(),
                            "operator 'in' is not supported for objects: an object always has the "
                            "fields of its type, so this would always be true");
    } else if (name) {
      checker_.diags_.error(bin->loc(), "operator 'in' is not supported for objects: type '" +
                                            object->name() + "' has no field '" + *name +
                                            "', so this would always be false");
    } else {
      checker_.diags_.error(bin->loc(),
                            "operator 'in' is not supported for objects: an object always has the "
                            "fields of its type");
    }
    return bin;
  }
  if (!r->nonOptional()->isArray()) {
    checker_.binaryChecker().reportOperandTypes(bin->loc(), "in", l, r);
    return bin;
  }
  std::string i = l->isInteger() && key ? *key : "i";
  std::string check =
      literalKey ? i + " < " + array + ".length" : i + " >= 0 && " + i + " < " + array + ".length";
  checker_.diags_.error(bin->loc(),
                        "operator 'in' is not supported for arrays, which have no holes; compare "
                        "the index with the length instead: '" +
                            check + "'");

  return bin;
}

} // namespace tz
