#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cctype>
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

TypeRelations::TypeRelations(Checker &checker) : checker_(checker) {}

bool TypeRelations::isImplicitlyConvertible(const Type *from, const Type *to) {
  if (from == to) {
    return true;
  }
  if (from->isNever()) {
    return true;
  }
  if (to->isUnknown()) {
    return !from->isVoid();
  }
  if (to->isAnyObject()) {
    return UnknownChecker::isObjectReference(from) ||
           (from->isParam() && from->constraint() &&
            isImplicitlyConvertible(from->constraint(), to));
  }
  if (to->isConstructor() && from->isClassRef()) {
    if (from->classRef() && from->classRef()->isGeneric()) {
      return false;
    }
    const Type *made = ClassValueChecker::constructedBy(from);
    return made ? isImplicitlyConvertible(made, to->constructed())
                : to->constructed()->isAnyObject();
  }
  if (to->isJson()) {
    return from->isString() || from->isNumeric() || from->isBool() || from->isLiteral() ||
           (from->isArray() && from->inner()->isJson());
  }
  if (to->isOptional()) {
    return from->isUndefined() || isImplicitlyConvertible(from->nonOptional(), to->inner());
  }
  if (to->isUnion()) {
    if (from->isUnion()) {
      return llvm::all_of(from->members(),
                          [&](const Type *m) { return LiteralTypes::unionMemberFor(m, to); });
    }
    return LiteralTypes::unionMemberFor(from, to) != nullptr;
  }
  if (from->isUnion()) {
    return !from->isTagged() && !hasParamMember(from) &&
           llvm::all_of(from->members(),
                        [&](const Type *m) { return isImplicitlyConvertible(m, to); });
  }
  if (to->isPattern()) {
    return from->isStringLiteral() && matchesPattern(from->literalText(), to);
  }
  if (from->isPattern()) {
    return to == Type::string();
  }
  if (from->isLiteral()) {
    return !to->isLiteral() && isImplicitlyConvertible(from->literalBase(), to);
  }
  if (from->isParam() && from->constraint()) {
    return isImplicitlyConvertible(from->constraint(), to);
  }
  if (from->isArray() && to->isArray()) {
    return WideWrites::sameReference(from->inner(), to->inner());
  }
  if (from->isIterator() && to->isIterator()) {
    return from->yielded() == to->yielded() &&
           (from->returned() == to->returned() || to->returned()->isVoid()) &&
           (from->sent() == to->sent() || to->sent()->isVoid());
  }
  if (from->isPromise() || to->isPromise()) {
    return from->isPromise() && to->isPromise() && from->promised()->isNever();
  }
  if (from->isBytes() || to->isBytes()) {
    return from->isBuffer() && to->bytes() == Type::Bytes::Uint8;
  }
  if (from->isMatch() || to->isMatch() || from->isRegExp() || to->isRegExp() || from->isDate() ||
      to->isDate() || from->isIntl() || to->isIntl() || from->isCrypto() || to->isCrypto() ||
      from->isWeakRef() || to->isWeakRef()) {
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 && !from->field("indices");
  }
  if (from->isObject() && to->isObject() && from->isSubtypeOf(to)) {
    return true;
  }
  if (from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
      from->isTuple() == to->isTuple() && from->fields().size() == to->fields().size() &&
      !from->isAnyObject() && !to->isAnyObject() && from->overloads().empty() &&
      to->overloads().empty()) {
    for (const Field &f : from->fields()) {
      const Field *t = to->field(f.name);
      if (!t || t->type != f.type) {
        return false;
      }
    }
    return true;
  }
  if (from->isResult() && to->isResult()) {
    auto half = [](const Type *a, const Type *b) {
      return a == b || a->isNever() ||
             (!a->isVoid() && !b->isVoid() && isImplicitlyConvertible(a, b));
    };
    return half(from->okType(), to->okType()) && half(from->errType(), to->errType());
  }

  return false;
}

const Type *TypeRelations::commonNumericType(const Type *a, const Type *b) {
  if (isImplicitlyConvertible(a, b)) {
    return b;
  }
  if (isImplicitlyConvertible(b, a)) {
    return a;
  }

  return nullptr;
}

bool TypeRelations::isIntLiteralTree(const Expr *expr) {
  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->var && name->var->literalConst;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return (unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus) &&
           isIntLiteralTree(unary->operand);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return (bin->op == BinaryOp::Add || bin->op == BinaryOp::Sub || bin->op == BinaryOp::Mul ||
            bin->op == BinaryOp::Rem || bin->op == BinaryOp::Pow) &&
           isIntLiteralTree(bin->lhs) && isIntLiteralTree(bin->rhs);
  }

  return false;
}

const Type *TypeRelations::literalTarget(const Expr *expr, const Type *to) {
  const Type *target = to->nonOptional();
  if (target->isUnion()) {
    target = LiteralTypes::numberMemberOf(target, false);
  }
  if (!target || !expr->type || !expr->type->isInteger() || expr->type == target ||
      !isIntLiteralTree(expr)) {
    return nullptr;
  }
  if (target->isFloat() || target->isDecimal()) {
    return target;
  }
  if (!target->isInteger()) {
    return nullptr;
  }
  std::optional<int64_t> value = checker_.constantFolder().intConstant(expr);
  if (!value) {
    return nullptr;
  }
  if (*value < 0 && !expr->type->isUnsigned()) {
    return !target->isUnsigned() && *value >= target->intMin() ? target : nullptr;
  }

  return uint64_t(*value) <= target->intMax() ? target : nullptr;
}

bool TypeRelations::literalAdapts(const Expr *expr, const Type *to) {
  return literalTarget(expr, to) != nullptr;
}

Expr *TypeRelations::adaptLiteral(Expr *expr, const Type *to) {
  const Type *target = literalTarget(expr, to);
  if (!target) {
    return expr;
  }
  Expr *adapted = convert(expr, target);

  return target == to ? adapted : convert(adapted, to);
}

std::string TypeRelations::operandText(llvm::StringRef text) {
  int depth = 0;
  bool plain = !text.empty() && text.front() != '-' && text.front() != '+';
  for (char c : text) {
    if (c == '(' || c == '[') {
      ++depth;
    } else if (c == ')' || c == ']') {
      --depth;
    } else if (depth == 0 && !std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' &&
               c != '#' && c != '$' && c != '"' && c != '\'') {
      plain = false;
    }
  }
  if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
    int level = 0;
    bool outer = true;
    for (size_t i = 0; i < text.size(); ++i) {
      level += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
      if (level == 0 && i + 1 < text.size()) {
        outer = false;
      }
    }
    plain |= outer;
  }
  bool literal = !text.empty() && std::isdigit(static_cast<unsigned char>(text.front()));

  return plain && !literal ? text.str() : "(" + text.str() + ")";
}

std::string TypeRelations::conversionHint(const Expr *expr, const Type *to) {
  const Type *from = expr->type ? expr->type->nonOptional() : nullptr;
  const Type *target = to->nonOptional();
  if (!from || !(from->isNumeric() || from->isDecimal()) ||
      !(target->isNumeric() || target->isDecimal())) {
    return "";
  }
  std::string text = "x";
  if (auto *name = dyn_cast<NameExpr>(expr); name && !name->name.starts_with("%")) {
    text = name->name.str();
  } else if (auto *member = dyn_cast<MemberExpr>(expr);
             member && isa<NameExpr>(member->object) &&
             !cast<NameExpr>(member->object)->name.starts_with("%")) {
    text = cast<NameExpr>(member->object)->name.str() + "." + member->name.str();
  }
  if (from->isInteger() && target->isInteger()) {
    return "; convert it explicitly: '" + text + "." + conversionMethod(target) + "()'";
  }
  if (from->isInteger() && target->isFloat()) {
    return "; convert it explicitly: '" + text + ".toDouble()'";
  }
  if (from->isInteger() && target->isDecimal()) {
    return "; convert it explicitly: '" + text + ".toDecimal()'";
  }
  if (from->isFloat() && target->isInteger()) {
    return "; round it first, then convert: 'Math.trunc(" + text + ")." + conversionMethod(target) +
           "()' (or Math.round, Math.floor, Math.ceil)";
  }

  return "";
}

std::string TypeRelations::conversionMethod(const Type *to) {
  if (to->isFloat()) {
    return "toDouble";
  }
  if (to->isDecimal()) {
    return "toDecimal";
  }
  if (to->isInt()) {
    return "toInt";
  }
  std::string name = to->name().str();
  name[0] = char(std::toupper(name[0]));

  return "to" + name;
}

const Type *TypeRelations::unify(const Type *a, const Type *b) {
  if (a == b) {
    return a;
  }
  if (a->isError() || b->isError()) {
    return Type::error();
  }
  if (a->isNever()) {
    return b;
  }
  if (b->isNever()) {
    return a;
  }
  if (a->isVoid() || b->isVoid()) {
    return nullptr;
  }
  if (a->isUndefined()) {
    return Type::optional(b);
  }
  if (b->isUndefined()) {
    return Type::optional(a);
  }
  if (a->isUnknown() || b->isUnknown()) {
    return Type::unknown();
  }
  const Type *x = a->nonOptional(), *y = b->nonOptional();
  if (x->isLiteral() || y->isLiteral() || x->isUnion() || y->isUnion()) {
    const Type *wider = isImplicitlyConvertible(x, y)   ? y
                        : isImplicitlyConvertible(y, x) ? x
                                                        : nullptr;
    if (!wider) {
      return nullptr;
    }
    return a->isOptional() || b->isOptional() ? Type::optional(wider) : wider;
  }
  if (x->isString() && y->isString()) {
    return a->isOptional() || b->isOptional() ? Type::optional(Type::string()) : Type::string();
  }
  auto number = [](const Type *t) { return t->isNumeric() || t->isDecimal(); };
  bool related = (number(x) && number(y)) || (x->isObject() && y->isObject()) ||
                 (x->isIterator() && y->isIterator());
  const Type *base = x == y ? x : related ? commonNumericType(x, y) : nullptr;
  if (!base && (x->isJson() || y->isJson())) {
    base = isImplicitlyConvertible(y, x) ? x : isImplicitlyConvertible(x, y) ? y : nullptr;
  }
  if (!base && x->isClass() && y->isClass()) {
    base = ClassLinks::commonClass(x, y);
  }
  if (!base && x->isArray() && y->isArray()) {
    base = isImplicitlyConvertible(x, y) ? y : isImplicitlyConvertible(y, x) ? x : nullptr;
  }
  if (!base && x->isPromise() && y->isPromise()) {
    base = x->promised()->isNever() ? y : y->promised()->isNever() ? x : nullptr;
  }
  if (!base && x->isClassRef() && y->isClassRef()) {
    const Type *p = ClassValueChecker::constructedBy(x), *q = ClassValueChecker::constructedBy(y);
    const Type *made = !p || !q                        ? nullptr
                       : isImplicitlyConvertible(p, q) ? q
                       : isImplicitlyConvertible(q, p) ? p
                                                       : ClassLinks::commonClass(p, q);
    base = Type::constructorOf(made ? made : Type::anyObject());
  }
  if (!base && (x->isAnyObject() || y->isAnyObject()) && UnknownChecker::isObjectReference(x) &&
      UnknownChecker::isObjectReference(y)) {
    base = Type::anyObject();
  }
  if (x->isResult() && y->isResult() && x != y) {
    const Type *value = ResultTypes::unifyHalf(x->okType(), y->okType());
    const Type *error = ResultTypes::unifyHalf(x->errType(), y->errType());
    base = value && error ? Type::result(value, error) : nullptr;
  }
  if (!base) {
    return nullptr;
  }

  return a->isOptional() || b->isOptional() ? Type::optional(base) : base;
}

Expr *TypeRelations::convert(Expr *expr, const Type *to) {
  if (expr->type == to || expr->type->isError() || to->isError()) {
    return expr;
  }
  const Type *reflected = to->nonOptional();
  bool reflects = reflected->isConstructor() || reflected->isAnyObject() ||
                  reflected->isUnknown() ||
                  (reflected->isArray() && reflected->inner()->isUnknown());
  if (expr->type->isParam() && !mentionsParam(to)) {
    checker_.genericChecker().noteForced(expr, to);
  }
  if ((to->isNumeric() || to->isDecimal()) && expr->type->isInteger()) {
    auto *unary = dyn_cast<UnaryExpr>(expr);
    auto *lit = dyn_cast<IntLitExpr>(unary && unary->op == UnaryOp::Neg ? unary->operand : expr);
    if (lit) {
      lit->type = to;
      if (unary) {
        unary->type = to;
      }
      return expr;
    }
  }
  const Type *from = expr->type->nonOptional(), *target = to->nonOptional();
  while (from->isArray() && target->isArray()) {
    from = from->inner()->nonOptional();
    target = target->inner()->nonOptional();
  }
  if (from->isObject() && target->isObject() && from != target && !from->isClass() &&
      !target->isClass() && !from->isAnyObject() && !target->isAnyObject() && !from->isBytes() &&
      !from->isSubtypeOf(target) &&
      !llvm::is_contained(checker_.file_->views,
                          std::pair<const Type *, const Type *>(from, target))) {
    checker_.file_->views.push_back({from, target});
  }
  auto *cast = checker_.ctx_.make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;
  if (checker_.wideWrites().widensWrites(expr->type, to)) {
    checker_.widenings_.insert(cast);
  }
  if (reflects) {
    checker_.reflectChecker().noteReflectedConversion(expr->type, to, cast);
  }
  checker_.reflectChecker().noteConstructorFlow(expr->type, to);

  return cast;
}

Expr *TypeRelations::coerce(Expr *expr, const Type *to, llvm::StringRef what) {
  const Type *from = expr->type;
  if (from == to || from->isError() || to->isError()) {
    return expr;
  }
  if (isImplicitlyConvertible(from, to)) {
    return convert(expr, to);
  }
  if (literalAdapts(expr, to)) {
    return adaptLiteral(expr, to);
  }
  if (checker_.conditionalTypes().fitsConditional(from, to)) {
    return convert(expr, to);
  }
  if (to->nonOptional()->isIterator() && from->isObject()) {
    Expr *iterator = checker_.iterableChecker().iteratorOf(expr);
    if (!iterator) {
      iterator = checker_.iterableChecker().userIterator(expr);
    }
    if (iterator && iterator->type->isIterator() && isImplicitlyConvertible(iterator->type, to)) {
      return convert(iterator, to);
    }
  }
  if (to->nonOptional()->isAsyncIterator() && from->isObject() && !from->isAsyncIterator()) {
    if (Expr *iterator = checker_.iterableChecker().asyncUserIterator(expr);
        iterator && iterator->type->isAsyncIterator() &&
        isImplicitlyConvertible(iterator->type, to)) {
      return convert(iterator, to);
    }
  }
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  if (from->isFunction() && function->isFunction()) {
    if (Expr *adapted = checker_.functionValueChecker().adaptFunction(expr, function)) {
      return function == to ? adapted : convert(adapted, to);
    }
    if (Expr *adapted = checker_.tzModulesChecker().adaptResult(expr, function)) {
      return function == to ? adapted : convert(adapted, to);
    }
  }

  std::optional<std::pair<int64_t, int64_t>> range = LiteralTypes::intRange(to->nonOptional());
  if (range && from->isInt() &&
      checker_.byteProofs().provesFits(expr, range->first, range->second)) {
    return convert(expr, to);
  }

  std::string message =
      ("type '" + from->name() + "' is not assignable to type '" + to->name() + "'").str();
  if (!what.empty()) {
    message += (" of " + what).str();
  }
  if (range && (from->isInt() || (from->isLiteral() && from->literalBase()->isInt()))) {
    std::string lo = std::to_string(range->first), hi = std::to_string(range->second);
    message += ", which holds " + lo + " to " + hi;
    if (from->isInt()) {
      std::string text = "x";
      if (auto *name = dyn_cast<NameExpr>(expr)) {
        text = name->name.str();
      }
      message += "; the value may be outside: check it first, e.g. 'if (" +
                 (to->nonOptional()->name() == "HttpStatus"
                      ? "isHttpStatus(" + text + ")"
                      : text + " >= " + lo + " && " + text + " <= " + hi) +
                 ")'";
    }
    checker_.diags_.error(expr->loc(), message);
    return expr;
  }
  const Type *fromObject = from->nonOptional(), *toObject = to->nonOptional();
  while (fromObject->isArray() && toObject->isArray()) {
    fromObject = fromObject->inner()->nonOptional();
    toObject = toObject->inner()->nonOptional();
  }
  if (from->isFunction() && function->isFunction()) {
    message += "; the types of functions must match exactly: wrap it in an arrow that converts "
               "what it needs";
  }
  const std::vector<std::pair<const Type *, llvm::SMLoc>> *places = nullptr;
  const Type *otherPlace = nullptr;
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var) {
    if (auto found = checker_.destinations_.find(name->var);
        found != checker_.destinations_.end()) {
      places = &found->second;
      for (auto [type, loc] : *places) {
        if (type->nonOptional() != toObject && !otherPlace) {
          otherPlace = type->nonOptional();
        }
      }
    }
  }
  const ClassDecl *fromClass = fromObject->classDecl();
  if (from->classRef() && from->classRef()->isGeneric() && toObject->isConstructor()) {
    const ClassDecl *generic = from->classRef();
    std::string name = generic->name.str();
    std::string example = generic->library ? "class Bus extends " + name + " {}"
                                           : "class Int" + name + " extends " + name + "<int> {}";
    message += "; each list of type arguments of '" + name +
               "' makes a class of its own, and 'new' of this value could make none of them: "
               "give a class that extends it, e.g. '" +
               example + "', or take the class as an 'object'";
  } else if (from->isUnknown() || from->isAnyObject()) {
    message += checker_.unknownChecker().unknownHint(expr, to);
  } else if (fromObject->isObject() && toObject->isClass() && (!fromClass || fromClass->literal)) {
    message += "; an object literal is never an instance of a class: create one with 'new " +
               toObject->name().str() + "(...)'";
  } else if (fromClass && fromClass->literal && toObject->isObject()) {
    message += "; an object literal with methods has a type of its own; to give it this type, "
               "write its functions as fields, 'f: () => ...'";
  } else if (fromObject->isClass() && toObject->isObject() && fromObject != toObject) {
    message += toObject->isClass()
                   ? "; classes are told apart by their names, whatever their fields, and '" +
                         fromObject->name().str() + "' does not extend '" + toObject->name().str() +
                         "'"
                   : "; an instance of a class is only of its class's type and of those it "
                     "extends or implements: write 'implements " +
                         toObject->name().str() + "' in the class";
  } else if (fromObject->isObject() && !fromObject->isClass() && toObject->isUnion() &&
             llvm::any_of(toObject->members(), [](const Type *m) { return m->isObject(); })) {
    message += "; an object literal goes into a union as one of its members: it needs the "
               "fields of one of them";
    const Type *front = toObject->members().front();
    for (const Field &f : front->isObject() ? front->fields() : llvm::ArrayRef<Field>()) {
      if (llvm::all_of(toObject->members(), [&](const Type *m) {
            return m->isObject() && m->field(f.name) && m->field(f.name)->type->isLiteral();
          })) {
        message += ", with its value of '" + f.name + "'";
        break;
      }
    }
  } else if (fromObject->isTuple() && toObject->isArray()) {
    message +=
        "; a tuple is not an array, though it is written like one: copy it into an array with "
        "'[...t]'";
  } else if (fromObject->isObject() && toObject->isObject() &&
             !isImplicitlyConvertible(fromObject, toObject)) {
    message +=
        checker_.objectChecker().objectMismatch(fromObject, toObject, otherPlace != nullptr) +
        (otherPlace ? "; the same object also goes where '" + otherPlace->name().str() +
                          "' is expected, and one object cannot be both"
                    : "");
  } else if (from->isOptional() && isImplicitlyConvertible(from->inner(), to)) {
    message += "; the value may be undefined: check it first (e.g. 'x != undefined') or give a "
               "default with '??'";
  } else if (from->nonOptional()->isJson()) {
    message += "; a Json may hold any JSON value: check what it holds first (e.g. 'typeof x == "
               "\"string\"' or 'Array.isArray(x)'), or convert it with 'fromJson<" +
               to->nonOptional()->name().str() + ">(x)'";
  } else if (to->nonOptional()->isAnyObject()) {
    message += "; only objects of the language are 'object's (instances of classes, object "
               "literals, dates, dictionaries...); for any value at all there is 'unknown'";
  } else if (to->nonOptional()->isJson() &&
             (fromObject->isObject() || from->nonOptional()->isArray())) {
    message += "; an array or an object becomes a Json only as a literal written where a Json is "
               "expected ('[...]', '{ ... }'), or as a 'Json[]', which stays the same array";
  } else if (to->nonOptional()->isJson() && from->nonOptional()->isDecimal()) {
    message += "; JSON has no decimals: write it as text with 'String(x)', as JSON.stringify does";
  } else if (from->isFloat() && to->isDecimal()) {
    message += "; a double is inexact already, so it becomes a decimal only explicitly: "
               "'decimal(x)'";
  } else if (from->isDecimal() && (to->isNumeric() || to->nonOptional()->isNumeric())) {
    message += "; a decimal becomes a number only explicitly: 'Number(x)' gives the nearest double";
  } else {
    message += conversionHint(expr, to);
  }
  checker_.diags_.error(expr->loc(), message);
  if (otherPlace) {
    for (auto [type, loc] : *places) {
      if (type->nonOptional() == otherPlace) {
        checker_.diags_.note(loc, "the object also goes here, as '" + otherPlace->name() + "'");
        break;
      }
    }
  }

  return expr;
}

const Type *TypeRelations::comparisonType(const Type *a, const Type *b) {
  const Type *common = a->isVoid() || b->isVoid() ? nullptr : unify(a, b);
  if (common && common->isError()) {
    return common;
  }
  const Type *base = common ? common->nonOptional() : nullptr;
  if (base && (base->isNumeric() || base->isDecimal() || base->isBool() || base->isString() ||
               base->isArray() || base->isFunction() || base->isObject() || base->isIterator() ||
               base->isJson() || base->isDict() || base->isUnknown() || base->isClassRef() ||
               base->isLiteral() || (base->isUnion() && !hasParamMember(base)))) {
    return common;
  }

  return nullptr;
}

}
