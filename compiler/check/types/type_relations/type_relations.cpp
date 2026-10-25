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
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
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

// Why an int's `x >>> n` may not go back into an int (see checkBinary).
static const char kUnsignedShift[] =
    "'>>>' is unsigned in JavaScript, and with a count of 0 a negative int becomes a number above "
    "int: keep the result in a long";

TypeRelations::TypeRelations(Checker &checker) : checker_(checker) {}

bool TypeRelations::isImplicitlyConvertible(const Type *from, const Type *to) {
  if (from == to) {
    return true;
  }
  // A value that cannot exist goes anywhere: `ok(1)` is a `Result<int, never>`,
  // which fits where a `Result<int, string>` is expected.
  if (from->isNever()) {
    return true;
  }
  // Widening, as in C#. A long beyond 2^53 loses precision as a double, as
  // every such number does in JavaScript.
  if (from == Type::intTy() && (to == Type::longTy() || to == Type::doubleTy())) {
    return true;
  }
  if (from == Type::longTy() && to == Type::doubleTy()) {
    return true;
  }
  // An integer becomes a decimal exactly; a double, inexact already, does not.
  if (from->isInteger() && to->isDecimal()) {
    return true;
  }
  // Any value is an unknown, and any object of the language an object
  // (docs/decorators.md): the same reference, or a box (see sema_unknown.cpp).
  if (to->isUnknown()) {
    return !from->isVoid();
  }
  if (to->isAnyObject()) {
    return UnknownChecker::isObjectReference(from) ||
           (from->isParam() && from->constraint() &&
            isImplicitlyConvertible(from->constraint(), to));
  }
  // A class as a value is a Constructor of what its objects are, and a
  // Constructor of a narrower type one of a wider type (docs/decorators.md,
  // section 5): the same reference to the object of the class.
  if (to->isConstructor() && from->isClassRef()) {
    // Not a generic class: each list of its type arguments makes a class of
    // its own, and `new` of the value could make none of them.
    if (from->classRef() && from->classRef()->isGeneric()) {
      return false;
    }
    const Type *made = ClassValueChecker::constructedBy(from);
    return made ? isImplicitlyConvertible(made, to->constructed())
                : to->constructed()->isAnyObject();
  }
  // A string, a number and a boolean are Json values, and so is a Json[],
  // which stays the same array (docs/modules.md, section 5).
  if (to->isJson()) {
    return from->isString() || from->isNumeric() || from->isBool() || from->isLiteral() ||
           (from->isArray() && from->inner()->isJson());
  }
  if (to->isOptional()) {
    return from->isUndefined() || isImplicitlyConvertible(from->nonOptional(), to->inner());
  }
  // A member goes into its union, and a union into one that has each of its
  // members (docs/objects.md, section 7).
  if (to->isUnion()) {
    if (from->isUnion()) {
      return llvm::all_of(from->members(),
                          [&](const Type *m) { return LiteralTypes::unionMemberFor(m, to); });
    }
    return LiteralTypes::unionMemberFor(from, to) != nullptr;
  }
  // A union goes where each of its members goes, if its value needs no tag:
  // `"up" | "down"` is a string, `Circle | Square` a Shape they implement.
  if (from->isUnion()) {
    return !from->isTagged() && !hasParamMember(from) &&
           llvm::all_of(from->members(),
                        [&](const Type *m) { return isImplicitlyConvertible(m, to); });
  }
  // A string of a pattern, `` `on${string}` ``, is a string, and a string
  // literal is one of a pattern it matches (docs/generics.md, section 10).
  if (to->isPattern()) {
    return from->isStringLiteral() && matchesPattern(from->literalText(), to);
  }
  if (from->isPattern()) {
    return to == Type::string();
  }
  // A literal is a value of its base type: `"circle"` is a string.
  if (from->isLiteral()) {
    return !to->isLiteral() && isImplicitlyConvertible(from->literalBase(), to);
  }
  // A value of `T extends C` is a C.
  if (from->isParam() && from->constraint()) {
    return isImplicitlyConvertible(from->constraint(), to);
  }
  // An array of Dog is an array of Animal: the same array. Code that puts
  // another Animal into it is found by the check of the whole program
  // (sema_widen.cpp, docs/objects.md, section 2).
  if (from->isArray() && to->isArray()) {
    return WideWrites::sameReference(from->inner(), to->inner());
  }
  // An iterator may be seen through a type that drops what its generator
  // returns, or that sends nothing to it; the values it gives are the same.
  if (from->isIterator() && to->isIterator()) {
    return from->yielded() == to->yielded() &&
           (from->returned() == to->returned() || to->returned()->isVoid()) &&
           (from->sent() == to->sent() || to->sent()->isVoid());
  }
  // A Promise that is never fulfilled, as Promise.reject() gives, is a
  // Promise of anything.
  if (from->isPromise() || to->isPromise()) {
    return from->isPromise() && to->isPromise() && from->promised()->isNever();
  }
  // A Buffer is a Uint8Array, as in node; the other objects of binary data
  // are only themselves.
  if (from->isBytes() || to->isBytes()) {
    return from->isBuffer() && to->bytes() == Type::Bytes::Uint8;
  }
  // A match of a known regex is a match of some regex: every match without
  // indices has the same layout.
  if (from->isMatch() || to->isMatch() || from->isRegExp() || to->isRegExp() || from->isDate() ||
      to->isDate() || from->isIntl() || to->isIntl() || from->isCrypto() || to->isCrypto() ||
      from->isWeakRef() || to->isWeakRef()) {
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 && !from->field("indices");
  }
  // An object is one of the types up its declared chain: the class it
  // extends, the interfaces it implements (docs/objects.md, section 2). The
  // reference stays as it is.
  if (from->isObject() && to->isObject() && from->isSubtypeOf(to)) {
    return true;
  }
  // An object may be seen through a type that makes some of its fields
  // read-only, or lets code change those that are; the fields are the same,
  // so nothing changes in memory (see Checker::convert for where they lie).
  // Code that changes a read-only one is found by the check of the whole
  // program (sema_widen.cpp). A class is only itself.
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
  // A Result is a value: each half converts, `Result<int, never>` to
  // `Result<double, string>`.
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
  // Anything goes into an unknown.
  if (a->isUnknown() || b->isUnknown()) {
    return Type::unknown();
  }
  const Type *x = a->nonOptional(), *y = b->nonOptional();
  // A literal and its base, a member and its union: the wider one.
  if (x->isLiteral() || y->isLiteral() || x->isUnion() || y->isUnion()) {
    const Type *wider = isImplicitlyConvertible(x, y)   ? y
                        : isImplicitlyConvertible(y, x) ? x
                                                        : nullptr;
    if (!wider) {
      return nullptr;
    }
    return a->isOptional() || b->isOptional() ? Type::optional(wider) : wider;
  }
  // Strings of patterns, and a string: a string.
  if (x->isString() && y->isString()) {
    return a->isOptional() || b->isOptional() ? Type::optional(Type::string()) : Type::string();
  }
  // An object and the same object with read-only fields give the read-only
  // one.
  auto number = [](const Type *t) { return t->isNumeric() || t->isDecimal(); };
  bool related = (number(x) && number(y)) || (x->isObject() && y->isObject()) ||
                 (x->isIterator() && y->isIterator());
  const Type *base = x == y ? x : related ? commonNumericType(x, y) : nullptr;
  // A Json and a value that is one: `j.port ?? 8080`.
  if (!base && (x->isJson() || y->isJson())) {
    base = isImplicitlyConvertible(y, x) ? x : isImplicitlyConvertible(x, y) ? y : nullptr;
  }
  // A Dog and a Cat are both Animals.
  if (!base && x->isClass() && y->isClass()) {
    base = ClassLinks::commonClass(x, y);
  }
  // An array of Dog and an array of Animal: the wider, which the check of
  // writes through it follows (sema_widen.cpp).
  if (!base && x->isArray() && y->isArray()) {
    base = isImplicitlyConvertible(x, y) ? y : isImplicitlyConvertible(y, x) ? x : nullptr;
  }
  if (!base && x->isPromise() && y->isPromise()) {
    base = x->promised()->isNever() ? y : y->promised()->isNever() ? x : nullptr;
  }
  // Two classes as values make a Constructor of what both make: `[Dog,
  // Cat]` is a `Constructor<Animal>[]`.
  if (!base && x->isClassRef() && y->isClassRef()) {
    const Type *p = ClassValueChecker::constructedBy(x), *q = ClassValueChecker::constructedBy(y);
    const Type *made = !p || !q                        ? nullptr
                       : isImplicitlyConvertible(p, q) ? q
                       : isImplicitlyConvertible(q, p) ? p
                                                       : ClassLinks::commonClass(p, q);
    base = Type::constructorOf(made ? made : Type::anyObject());
  }
  // A class as a value and an object are objects.
  if (!base && (x->isAnyObject() || y->isAnyObject()) && UnknownChecker::isObjectReference(x) &&
      UnknownChecker::isObjectReference(y)) {
    base = Type::anyObject();
  }
  // `ok(1)` and `err("empty")` make a `Result<int, string>`.
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
  // What reflection may reach (see sema_reflect.cpp), through the conversion
  // made below.
  const Type *reflected = to->nonOptional();
  bool reflects = reflected->isConstructor() || reflected->isAnyObject() ||
                  reflected->isUnknown() ||
                  (reflected->isArray() && reflected->inner()->isUnknown());
  // Generic code converts a value of type T here; instances will too.
  if (expr->type->isParam() && !mentionsParam(to)) {
    checker_.genericChecker().noteForced(expr, to);
  }
  // An integer literal becomes a literal of the wider type, so that `-0` in
  // `[1.5, -0]` stays a negative zero (and a decimal one is a constant).
  if ((to->isNumeric() || to->isDecimal()) && expr->type->isInteger() &&
      isImplicitlyConvertible(expr->type, to)) {
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
  // An object seen through a type with read-only fields: Lowering reads them
  // as through an interface that the object's type implements, since the
  // types may lie out differently (see SourceFile::views). A Buffer is a
  // Uint8Array as it is.
  const Type *from = expr->type->nonOptional(), *target = to->nonOptional();
  // So are the elements of an array seen as an array of such objects.
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
  // A conversion that lets code write what the value's type does not, as
  // `Dog[]` to `Animal[]`: checked against the writes of the program once it
  // is all checked (sema_widen.cpp).
  if (checker_.wideWrites().widensWrites(expr->type, to)) {
    checker_.widenings_.insert(cast);
  }
  if (reflects) {
    checker_.reflectChecker().noteReflectedConversion(expr->type, to, cast);
  }

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
  // Generic code may give a value of either branch where a conditional type
  // that waits is expected; each copy checks the branch it chooses.
  if (checker_.conditionalTypes().fitsConditional(from, to)) {
    return convert(expr, to);
  }
  // An iterator written by hand (an object with next()), or an object with
  // `[Symbol.iterator]()`, where an iterator is expected.
  if (to->nonOptional()->isIterator() && from->isObject()) {
    Expr *iterator = checker_.iterableChecker().iteratorOf(expr);
    if (!iterator) {
      iterator = checker_.iterableChecker().userIterator(expr);
    }
    if (iterator && iterator->type->isIterator() && isImplicitlyConvertible(iterator->type, to)) {
      return convert(iterator, to);
    }
  }
  // An object with `[Symbol.asyncIterator]()` where an async iterator is
  // expected, as an `AsyncIterable<T>` parameter.
  if (to->nonOptional()->isAsyncIterator() && from->isObject() && !from->isAsyncIterator()) {
    if (Expr *iterator = checker_.iterableChecker().asyncUserIterator(expr);
        iterator && iterator->type->isAsyncIterator() &&
        isImplicitlyConvertible(iterator->type, to)) {
      return convert(iterator, to);
    }
  }
  // A function named here can be given a type that takes more or fewer
  // arguments.
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  if (from->isFunction() && function->isFunction()) {
    if (Expr *adapted = checker_.functionValueChecker().adaptFunction(expr, function)) {
      return function == to ? adapted : convert(adapted, to);
    }
    if (Expr *adapted = checker_.tzModulesChecker().adaptResult(expr, function)) {
      return function == to ? adapted : convert(adapted, to);
    }
  }

  std::string message =
      ("type '" + from->name() + "' is not assignable to type '" + to->name() + "'").str();
  if (!what.empty()) {
    message += (" of " + what).str();
  }
  // Objects, or arrays of them: the fields that differ.
  const Type *fromObject = from->nonOptional(), *toObject = to->nonOptional();
  while (fromObject->isArray() && toObject->isArray()) {
    fromObject = fromObject->inner()->nonOptional();
    toObject = toObject->inner()->nonOptional();
  }
  if (from->isFunction() && function->isFunction()) {
    message += "; the types of functions must match exactly: wrap it in an arrow that converts "
               "what it needs";
  }
  // A literal that goes to places whose types disagree (see
  // literalDestination).
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
    // An emitter of the library extends with its map inferred.
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
    // A literal goes into a union as one of its members.
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
    message += checker_.objectChecker().objectMismatch(fromObject, toObject,
                                                       /*conflict=*/otherPlace != nullptr) +
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
  } else if (from->isFloat() && to->isInt()) {
    message += "; drop the fraction with 'x | 0', or round with Math.round, Math.floor, "
               "Math.ceil or Math.trunc";
  } else if (from->isFloat() && to->isInteger()) {
    message += "; round it with Math.round, Math.floor, Math.ceil or Math.trunc";
  } else if (auto *bin = dyn_cast<BinaryExpr>(expr);
             bin && bin->op == BinaryOp::UShr && bin->lhs->type->isInt() && to->isInt()) {
    message += "; " + std::string(kUnsignedShift) +
               (bin->source.empty() ? "" : ", e.g. 'const u = " + bin->source.str() + "'");
  } else if (from->isInteger() && to->isInteger()) {
    message += "; Math.trunc(x) converts it and checks that it fits";
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

} // namespace tz
