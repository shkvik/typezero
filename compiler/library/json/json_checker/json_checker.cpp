#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/values/array_literal_checker/array_literal_checker.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// The variable a condition tests, looking through the conversion of a
// narrowed variable; null unless it is a Json (or a `Json | undefined`).
const VarSymbol *jsonVariable(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->var && name->var->type->nonOptional()->isJson() ? name->var : nullptr;
}

// The literal `null`: a name that no declaration has.
bool nullName(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->name == "null" && !name->var && !name->func;
}

// `null` or `undefined` (also `void x`), which a Json compares equal to both.
bool isNothing(const Expr *expr) {
  return nullName(expr) || (expr->type && expr->type->isUndefined());
}

} // namespace

JsonChecker::JsonChecker(Checker &checker) : checker_(checker) {}

bool JsonChecker::holdsJson(const Type *type) {
  while (type->isOptional() || type->isArray()) {
    type = type->inner();
  }

  return type->isJson();
}

bool JsonChecker::isNullName(const Expr *expr) { return nullName(expr); }

Expr *JsonChecker::checkNull(NameExpr *name, const Type *expected) {
  if (!isNullName(name) || !expected || !expected->nonOptional()->isJson()) {
    return nullptr;
  }
  name->type = Type::json();

  return name;
}

Expr *JsonChecker::checkJsonMember(MemberExpr *member) {
  member->type = Type::optional(Type::json());

  return member;
}

// A key is a string, or a number, which JavaScript turns into its text:
// `j[1]` of an object reads "1".
Expr *JsonChecker::checkJsonIndex(IndexExpr *index) {
  index->index = checker_.exprChecker().checkExpr(index->index);
  index->type = Type::optional(Type::json());
  const Type *key = index->index->type;
  if (key->isError() || key->isString()) {
    return index;
  }
  if (key->isNumeric()) {
    index->index = checker_.typeRelations().convert(index->index, Type::doubleTy());
    return index;
  }
  checker_.diags_.error(index->index->loc(),
                        "a key of a Json is a string or a number, found '" + key->name() + "'");
  index->type = Type::error();

  return index;
}

const Type *JsonChecker::checkJsonTarget(Expr *target) {
  if (auto *index = dyn_cast<IndexExpr>(target)) {
    if (checkJsonIndex(index)->type->isError()) {
      return Type::error();
    }
  } else {
    checkJsonMember(cast<MemberExpr>(target));
  }

  return Type::optional(Type::json());
}

Expr *JsonChecker::reportJsonMethod(CallExpr *call, MemberExpr *member) {
  const Expr *object = member->object;
  if (auto *conv = dyn_cast<CastExpr>(object); conv && conv->isImplicit()) {
    object = conv->operand;
  }
  std::string message =
      "a Json has no method '" + member->name.str() + "' until a check shows what it holds";
  std::string call_ = "." + member->name.str() + "(...)";
  if (auto *name = dyn_cast<NameExpr>(object); name && name->var) {
    std::string x = name->name.str();
    message += ", e.g. 'if (Array.isArray(" + x + ")) " + x + call_ + "' or 'if (typeof " + x +
               " == \"string\") " + x + call_ + "'";
  } else {
    message += "; put it in a variable and check that, e.g. 'const items = j.items; if "
               "(Array.isArray(items)) items" +
               call_ + "'";
  }
  checker_.diags_.error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

// `{ phone: null, tags: ["a"] }` where a Json is expected: a Json object.
// A value that is undefined leaves its key out, as JSON.stringify does, and
// a key in brackets may be any string.
Expr *JsonChecker::checkJsonObjectLit(ObjectLitExpr *lit) {
  const Type *json = Type::json();
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread || prop.method) {
      checker_.diags_.error(prop.loc,
                            prop.spread ? "a spread is not supported in a Json literal; write the "
                                          "keys one by one"
                                        : "a Json holds no functions");
      prop.value = checker_.exprChecker().checkExpr(prop.value);
      continue;
    }
    if (prop.defaulted) {
      checker_.diags_.error(prop.loc, "'" + prop.name +
                                          " = ...' belongs to a destructuring pattern; write '" +
                                          prop.name + ": ...'");
      prop.value = checker_.exprChecker().checkExpr(cast<AssignExpr>(prop.value)->value);
      prop.defaulted = false;
      continue;
    }
    if (prop.key) {
      prop.key = checker_.exprChecker().checkExpr(prop.key);
      if (!prop.key->type->isString() && !prop.key->type->isError()) {
        checker_.diags_.error(prop.key->loc(), "a key of a Json is a string, found '" +
                                                   prop.key->type->name() + "'");
      }
    }
    Expr *value = checker_.exprChecker().checkExpr(prop.value, json);
    const Type *type = value->type;
    prop.value = checker_.typeRelations().coerce(
        value, type->isOptional() || type->isUndefined() ? Type::optional(json) : json,
        "key '" + prop.name.str() + "'");
  }
  lit->type = json;

  return lit;
}

// `[1, "a", null]` where a Json is expected: a Json[], which becomes the Json.
Expr *JsonChecker::checkJsonArrayLit(ArrayLitExpr *lit) {
  Expr *array = checker_.arrayLiteralChecker().checkArrayLit(lit, Type::array(Type::json()));
  if (array->type->isError()) {
    return array;
  }

  return checker_.typeRelations().coerce(array, Type::json());
}

// `x == null` is true for null and undefined, as in JavaScript, and only a
// Json holds null.
Expr *JsonChecker::checkNullEquality(BinaryExpr *bin) {
  bool lhsNull = isNullName(bin->lhs), rhsNull = isNullName(bin->rhs);
  if (!lhsNull && !rhsNull) {
    return nullptr;
  }
  bin->type = Type::boolTy();
  Expr *&null = lhsNull ? bin->lhs : bin->rhs;
  Expr *&other = lhsNull ? bin->rhs : bin->lhs;
  const Type *type = Type::undefinedTy();
  if (!(lhsNull && rhsNull)) {
    other = checker_.exprChecker().checkExpr(other);
    type = other->type;
  }
  null->type = Type::json();
  if (type->isError() || type->nonOptional()->isJson()) {
    return bin;
  }
  checker_.diags_.error(null->loc(),
                        "'null' exists only in Json values, and a value of type '" + type->name() +
                            "' is never null; the one value for nothing is 'undefined': '" +
                            opSpelling(bin->op) + " undefined'");
  null->type = Type::error();

  return bin;
}

Expr *JsonChecker::checkJsonEquality(BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bool lhsJson = l->nonOptional()->isJson(), rhsJson = r->nonOptional()->isJson();
  const Type *json =
      l->isOptional() || r->isOptional() ? Type::optional(Type::json()) : Type::json();
  if (lhsJson && rhsJson) {
    bin->lhs = checker_.typeRelations().convert(bin->lhs, json);
    bin->rhs = checker_.typeRelations().convert(bin->rhs, json);
    return bin;
  }
  // `j == "admin"`, `j.count == 3`: as `===`, true for a Json of that kind
  // and value.
  Expr *&other = lhsJson ? bin->rhs : bin->lhs;
  const Type *type = other->type;
  if (type->isString() || type->isBool()) {
    return bin;
  }
  if (type->isNumeric()) {
    other = checker_.typeRelations().convert(other, Type::doubleTy());
    return bin;
  }
  if (type->isOptional() && TypeRelations::isImplicitlyConvertible(type->inner(), Type::json())) {
    bin->lhs = checker_.typeRelations().convert(bin->lhs, json);
    bin->rhs = checker_.typeRelations().convert(bin->rhs, json);
    return bin;
  }
  checker_.binaryChecker().reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);

  return bin;
}

void JsonChecker::addJsonFacts(const Expr *cond, Checker::Flow &whenTrue,
                               Checker::Flow &whenFalse) const {
  // `Array.isArray(x)`: x is a Json[].
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin == Builtin::ArrayIsArray) {
    if (const VarSymbol *var = jsonVariable(call->args[0])) {
      whenTrue.narrowed.push_back({var, Type::array(Type::json())});
      if (var->type->isOptional()) {
        whenTrue.defined.insert(var);
      }
    }
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin || (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne)) {
    return;
  }
  Checker::Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Checker::Flow &different = bin->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    // `x != null`, `x != undefined`: neither, so not undefined.
    if (isNothing(other)) {
      if (const VarSymbol *var = jsonVariable(side); var && var->type->isOptional()) {
        different.defined.insert(var);
      }
      continue;
    }
    // `typeof x == "string"`: a string, a number (a double) or a boolean.
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text) {
      continue;
    }
    const VarSymbol *var = jsonVariable(typeOf->operand);
    if (!var) {
      continue;
    }
    const Type *narrowed = text->value == u"string"    ? Type::string()
                           : text->value == u"number"  ? Type::doubleTy()
                           : text->value == u"boolean" ? Type::boolTy()
                                                       : nullptr;
    if (narrowed) {
      equal.narrowed.push_back({var, narrowed});
    }
    if (var->type->isOptional()) {
      if (narrowed || text->value == u"object") {
        equal.defined.insert(var);
      } else if (text->value == u"undefined") {
        different.defined.insert(var);
      }
    }
  }
}

Expr *JsonChecker::checkArrayIsArray(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
    return call;
  }
  call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
  if (call->args[0]->type->isVoid()) {
    checker_.diags_.error(call->args[0]->loc(),
                          "an expression of type 'void' has no value to test");
    return call;
  }
  call->builtin = Builtin::ArrayIsArray;
  call->type = Type::boolTy();

  return call;
}

} // namespace tz
