#include "compiler/check/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

DictNewChecker::DictNewChecker(Sema &sema) : sema_(sema) {}

// `[["a", 1], ["b", 2]]`, where nothing gives the types of a Map: its pairs
// say them. Returns false if `arg` is no such literal.
bool DictNewChecker::inferPairs(Expr *arg, const Type *&key, const Type *&value) {
  auto *lit = dyn_cast<ArrayLitExpr>(arg);
  if (!lit || lit->elements.empty()) {
    return false;
  }
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = dyn_cast<ArrayLitExpr>(e.expr);
    if (e.spread || !pair || pair->elements.size() != 2 || pair->elements[0].spread ||
        pair->elements[1].spread) {
      return false;
    }
  }
  key = value = nullptr;
  bool ok = true;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = cast<ArrayLitExpr>(e.expr);
    for (size_t i = 0; i < 2; ++i) {
      Expr *&part = pair->elements[i].expr;
      const Type *&common = i == 0 ? key : value;
      part = sema_.exprChecker().checkExpr(part, common && DecimalChecker::isLiteralTree(part) ? common : nullptr);
      const Type *t = part->type;
      if (t->isError()) {
        ok = false;
        continue;
      }
      const Type *next = common ? TypeRelations::unify(common, t) : t;
      if (!next) {
        sema_.diags_.error(part->loc(), std::string("the ") + (i == 0 ? "keys" : "values") +
                                      " of a Map have one type; found '" + common->name() +
                                      "' and '" + t->name() + "'");
        ok = false;
        continue;
      }
      common = next;
    }
  }
  if (!ok || !key || !value) {
    key = value = Type::error();
    return true;
  }
  if (key->isUndefined() || value->isUndefined() || key->isVoid() || value->isVoid()) {
    sema_.diags_.error(arg->loc(), "cannot infer the types of the Map from 'undefined'; give them, e.g. "
                             "'new Map<string, int | undefined>(...)'");
    key = value = Type::error();
    return true;
  }
  const Type *pairType = Type::tuple({key, value});
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = cast<ArrayLitExpr>(e.expr);
    pair->elements[0].expr = sema_.typeRelations().coerce(pair->elements[0].expr, key);
    pair->elements[1].expr = sema_.typeRelations().coerce(pair->elements[1].expr, value);
    pair->type = pairType;
  }
  lit->type = Type::array(pairType);

  return true;
}

// The entries a new Map takes ([key, value] pairs, with `pairs`), or the
// values of a new Set: whatever can be iterated. `element` is what each must
// be, null if nothing says yet; it becomes what each is. The result is the
// iterable: a Map or a Set as its iterator.
Expr *DictNewChecker::checkDictSource(Expr *arg, bool pairs, const Type *&element, llvm::StringRef what) {
  sema_.iteratorMethods().markFused(arg);
  arg = sema_.dictChecker().iterableOf(sema_.exprChecker().checkExpr(arg, element ? Type::array(element) : nullptr));
  const Type *type = arg->type;
  const Type *wanted = element;
  element = Type::error();
  if (type->isError()) {
    return arg;
  }
  const Type *given = GeneratorChecker::iteratedType(type);
  if (!given) {
    sema_.diags_.error(arg->loc(), what +
                                 " needs an array, a string, an iterator, a Map or a Set, found '" +
                                 type->name() + "'" +
                                 (type->isRecord() ? "; for the entries of a Record, pass "
                                                     "'Object.entries(r)'"
                                                   : ""));
    return arg;
  }
  if (type->isIterator()) {
    sema_.generatorChecker().checkSendsNothing(type, arg->loc(), what);
    sema_.flowChecker().calledFunction(); // a generator runs
  }
  if (pairs) {
    bool pair = given->isTuple() && given->fields().size() == 2;
    if (!pair ||
        (wanted && (!TypeRelations::isImplicitlyConvertible(given->fields()[0].type, wanted->fields()[0].type) ||
                    !TypeRelations::isImplicitlyConvertible(given->fields()[1].type, wanted->fields()[1].type)))) {
      sema_.diags_.error(arg->loc(),
                   what + " takes [key, value] pairs" +
                       (wanted ? " of '" + wanted->name().str() + "'" : std::string()) +
                       ", found '" + given->name() + "'" +
                       (given->isArray() ? "; an array literal is a pair only where one "
                                           "is expected: give the callback its type, "
                                           "e.g. '(x): [string, int] => [x.name, x.n]'"
                                         : ""));
      return arg;
    }
  } else if (wanted && !TypeRelations::isImplicitlyConvertible(given, wanted)) {
    sema_.diags_.error(arg->loc(), what + " takes values of type '" + wanted->name() + "', found '" +
                                 given->name() + "'");
    return arg;
  }
  element = given;

  return arg;
}

Expr *DictNewChecker::checkDictNew(NewExpr *expr, const Type *expected) {
  bool isMap = expr->name == "Map";
  llvm::SMLoc loc = expr->loc();
  if (expr->name == "WeakMap" || expr->name == "WeakSet") {
    TypeRef ref;
    ref.name = expr->name;
    ref.loc = loc;
    sema_.dictChecker().resolveDictType(ref); // reports it
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    expr->type = Type::error();
    return expr;
  }
  auto fail = [&] {
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    expr->type = Type::error();

    return expr;
  };
  const Type *type = nullptr;
  // Where a union is expected, its one Map (or Set), if it has one.
  if (expected && expected->nonOptional()->isUnion()) {
    const Type *only = nullptr;
    int count = 0;
    for (const Type *m : expected->nonOptional()->members()) {
      if (m->isDict() && !m->isRecord() && m->isMap() == isMap) {
        only = m;
        ++count;
      }
    }
    expected = count == 1 ? only : nullptr;
  }
  if (!expr->typeArgs.empty()) {
    TypeRef ref;
    ref.kind = TypeRef::Kind::Generic;
    ref.name = expr->name;
    ref.loc = loc;
    ref.args = expr->typeArgs;
    type = sema_.dictChecker().resolveDictType(ref);
    if (type->isError()) {
      return fail();
    }
  } else if (const Type *wanted = expected ? expected->nonOptional() : nullptr;
             wanted && wanted->isDict() && wanted->isMap() == isMap && !wanted->isRecord()) {
    type = wanted;
  }
  if (expr->args.size() > 1) {
    sema_.diags_.error(expr->args[1]->loc(), "'new " + expr->name + "' takes one argument, the " +
                                           (isMap ? "entries" : "values") + " to start with");
    return fail();
  }
  std::vector<Expr *> args;
  if (expr->args.empty()) {
    if (!type) {
      sema_.diags_.error(loc, isMap ? "'new Map()' needs the types of its keys and values, e.g. 'new "
                                "Map<string, int>()'"
                              : "'new Set()' needs the type of its elements, e.g. 'new "
                                "Set<int>()'");
      return fail();
    }
  } else if (Expr *arg = expr->args[0]; isa<UndefinedExpr>(arg)) {
    // `new Map(undefined)` starts empty, as JavaScript's does.
    if (!type) {
      sema_.diags_.error(
          loc, "'new " + expr->name + "(undefined)' needs the types of the " +
                   (isMap ? "Map, e.g. 'new Map<string, int>()'" : "Set, e.g. 'new Set<int>()'"));
      return fail();
    }
  } else {
    const Type *key = nullptr, *value = nullptr;
    if (!type && isMap && inferPairs(arg, key, value)) {
      if (key->isError() || !sema_.dictChecker().checkKeyType(key, arg->loc())) {
        expr->type = Type::error();
        return expr;
      }
      type = Type::dict(Type::DictKind::Map, key, value);
      args.push_back(arg);
    } else {
      const Type *element =
          type ? (isMap ? Type::tuple({type->keyType(), type->valueType()}) : type->keyType())
               : nullptr;
      arg = checkDictSource(arg, isMap, element, "'new " + expr->name.str() + "'");
      if (element->isError()) {
        expr->type = Type::error();
        return expr;
      }
      if (!type) {
        if (isMap && (!element->isTuple() || element->fields().size() != 2)) {
          sema_.diags_.error(arg->loc(),
                       "'new Map' takes [key, value] pairs, found '" + element->name() + "'");
          expr->type = Type::error();
          return expr;
        }
        const Type *k = isMap ? element->fields()[0].type : element;
        if (!sema_.dictChecker().checkKeyType(k, arg->loc())) {
          expr->type = Type::error();
          return expr;
        }
        if (k->isUndefined() || (isMap && element->fields()[1].type->isUndefined())) {
          sema_.diags_.error(arg->loc(), "cannot infer the types of the " + expr->name.str() +
                                       " from 'undefined'; give them");
          expr->type = Type::error();
          return expr;
        }
        type = isMap ? Type::dict(Type::DictKind::Map, k, element->fields()[1].type)
                     : Type::dict(Type::DictKind::Set, k, Type::voidTy());
      }
      args.push_back(arg);
    }
  }
  if (!sema_.dictChecker().checkKeyType(type->keyType(), loc)) {
    return fail();
  }
  auto *call = sema_.ctx_.make<CallExpr>(loc, sema_.ctx_.make<NameExpr>(loc, expr->name), std::move(args));
  call->callee->type = type;
  call->builtin = Builtin::DictNew;
  call->type = type;

  return call;
}

} // namespace tz
