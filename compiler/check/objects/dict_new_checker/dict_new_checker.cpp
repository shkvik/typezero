#include "compiler/check/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

DictNewChecker::DictNewChecker(Checker &owner)
    : checker(owner) {}

void DictNewChecker::markPairs(Expr *arg) {
  auto *call = dyn_cast<CallExpr>(arg);
  auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  auto *fe = call && call->args.size() == 1 ? dyn_cast<FunctionExpr>(call->args[0]) : nullptr;
  if (!member || member->name != "map" || !fe || fe->fn->returnType || !fe->fn->body) {
    return;
  }
  std::vector<const Expr *> returned;
  bool plain = true;
  std::function<void(const Stmt *)> walk = [&](const Stmt *s) {
    if (!s) {
      return;
    }
    if (auto *ret = dyn_cast<ReturnStmt>(s)) {
      auto *pair = dyn_cast_or_null<ArrayLitExpr>(ret->value);
      if (!pair || pair->elements.size() != 2 || pair->elements[0].spread ||
          pair->elements[1].spread) {
        plain = false;
        return;
      }
      returned.push_back(pair);
    } else if (auto *block = dyn_cast<BlockStmt>(s)) {
      for (const Stmt *inner : block->body) {
        walk(inner);
      }
    } else if (auto *branch = dyn_cast<IfStmt>(s)) {
      walk(branch->thenStmt);
      walk(branch->elseStmt);
    }
  };
  walk(fe->fn->body);
  if (!plain || returned.empty()) {
    return;
  }
  for (const Expr *pair : returned) {
    this->checker.pairLiterals_.insert(pair);
  }
}

bool DictNewChecker::inferPairs(Expr *arg, const Type *&key, const Type *&value) {
  this->markPairs(arg);
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
      part = this->checker.exprChecker().checkExpr(
          part, common && DecimalChecker::isLiteralTree(part) ? common : nullptr);
      const Type *t = part->type;
      if (t->isError()) {
        ok = false;
        continue;
      }
      const Type *next = common ? TypeRelations::unify(common, t) : t;
      if (!next) {
        this->checker.diags_.error(part->loc(), std::string("the ") + (i == 0 ? "keys" : "values") +
                                                    " of a Map have one type; found '" +
                                                    common->name() + "' and '" + t->name() + "'");
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
    this->checker.diags_.error(
        arg->loc(), "cannot infer the types of the Map from 'undefined'; give them, e.g. "
                    "'new Map<string, int | undefined>(...)'");
    key = value = Type::error();
    return true;
  }
  const Type *pairType = Type::tuple({key, value});
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = cast<ArrayLitExpr>(e.expr);
    pair->elements[0].expr = this->checker.typeRelations().coerce(pair->elements[0].expr, key);
    pair->elements[1].expr = this->checker.typeRelations().coerce(pair->elements[1].expr, value);
    pair->type = pairType;
  }
  lit->type = Type::array(pairType);

  return true;
}

Expr *DictNewChecker::checkDictSource(Expr *arg, bool pairs, const Type *&element,
                                      llvm::StringRef what) {
  this->checker.iteratorMethods().markFused(arg);
  arg = this->checker.dictChecker().iterableOf(
      this->checker.exprChecker().checkExpr(arg, element ? Type::array(element) : nullptr));
  const Type *type = arg->type;
  const Type *wanted = element;
  element = Type::error();
  if (type->isError()) {
    return arg;
  }
  const Type *given = GeneratorChecker::iteratedType(type);
  if (!given) {
    this->checker.diags_.error(
        arg->loc(), what + " needs an array, a string, an iterator, a Map or a Set, found '" +
                        type->name() + "'" +
                        (type->isRecord() ? "; for the entries of a Record, pass "
                                            "'Object.entries(r)'"
                                          : ""));
    return arg;
  }
  if (type->isIterator()) {
    this->checker.generatorChecker().checkSendsNothing(type, arg->loc(), what);
    this->checker.flowChecker().calledFunction();
  }
  if (pairs) {
    bool pair = given->isTuple() && given->fields().size() == 2;
    if (!pair || (wanted && (!TypeRelations::isImplicitlyConvertible(given->fields()[0].type,
                                                                     wanted->fields()[0].type) ||
                             !TypeRelations::isImplicitlyConvertible(given->fields()[1].type,
                                                                     wanted->fields()[1].type)))) {
      this->checker.diags_.error(
          arg->loc(), what + " takes [key, value] pairs" +
                          (wanted ? " of '" + wanted->name().str() + "'" : std::string()) +
                          ", found '" + given->name() + "'" +
                          (given->isArray() ? "; an array literal is a pair only where one "
                                              "is expected: give the callback its type, "
                                              "e.g. '(x): [string, int] => [x.name, x.n]'"
                                            : ""));
      return arg;
    }
  } else if (wanted && !TypeRelations::isImplicitlyConvertible(given, wanted)) {
    this->checker.diags_.error(arg->loc(), what + " takes values of type '" + wanted->name() +
                                               "', found '" + given->name() + "'");
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
    this->checker.dictChecker().resolveDictType(ref);
    for (Expr *&arg : expr->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    expr->type = Type::error();
    return expr;
  }
  auto fail = [&] {
    for (Expr *&arg : expr->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    expr->type = Type::error();

    return expr;
  };
  const Type *type = nullptr;
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
    ref.form = GenericTypeRef{};
    ref.name = expr->name;
    ref.loc = loc;
    ref.args = expr->typeArgs;
    type = this->checker.dictChecker().resolveDictType(ref);
    if (type->isError()) {
      return fail();
    }
  } else if (const Type *wanted = expected ? expected->nonOptional() : nullptr;
             wanted && wanted->isDict() && wanted->isMap() == isMap && !wanted->isRecord()) {
    type = wanted;
  }
  if (expr->args.size() > 1) {
    this->checker.diags_.error(expr->args[1]->loc(),
                               "'new " + expr->name + "' takes one argument, the " +
                                   (isMap ? "entries" : "values") + " to start with");
    return fail();
  }
  std::vector<Expr *> args;
  if (expr->args.empty()) {
    if (!type) {
      this->checker.diags_.error(
          loc, isMap ? "'new Map()' needs the types of its keys and values, e.g. 'new "
                       "Map<string, int>()'"
                     : "'new Set()' needs the type of its elements, e.g. 'new "
                       "Set<int>()'");
      return fail();
    }
  } else if (Expr *arg = expr->args[0]; isa<UndefinedExpr>(arg)) {
    if (!type) {
      this->checker.diags_.error(
          loc, "'new " + expr->name + "(undefined)' needs the types of the " +
                   (isMap ? "Map, e.g. 'new Map<string, int>()'" : "Set, e.g. 'new Set<int>()'"));
      return fail();
    }
  } else {
    const Type *key = nullptr;
    const Type *value = nullptr;
    if (!type && isMap && this->inferPairs(arg, key, value)) {
      if (key->isError() || !this->checker.dictChecker().checkKeyType(key, arg->loc())) {
        expr->type = Type::error();
        return expr;
      }
      type = Type::dict(Type::DictKind::Map, key, value);
      args.push_back(arg);
    } else {
      const Type *element =
          type ? (isMap ? Type::tuple({type->keyType(), type->valueType()}) : type->keyType())
               : nullptr;
      arg = this->checkDictSource(arg, isMap, element, "'new " + expr->name.str() + "'");
      if (element->isError()) {
        expr->type = Type::error();
        return expr;
      }
      if (!type) {
        if (isMap && (!element->isTuple() || element->fields().size() != 2)) {
          this->checker.diags_.error(arg->loc(), "'new Map' takes [key, value] pairs, found '" +
                                                     element->name() + "'");
          expr->type = Type::error();
          return expr;
        }
        const Type *k = isMap ? element->fields()[0].type : element;
        if (!this->checker.dictChecker().checkKeyType(k, arg->loc())) {
          expr->type = Type::error();
          return expr;
        }
        if (k->isUndefined() || (isMap && element->fields()[1].type->isUndefined())) {
          this->checker.diags_.error(arg->loc(), "cannot infer the types of the " +
                                                     expr->name.str() +
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
  if (!this->checker.dictChecker().checkKeyType(type->keyType(), loc)) {
    return fail();
  }
  auto *call = this->checker.ctx_.make<CallExpr>(
      loc, this->checker.ctx_.make<NameExpr>(loc, expr->name), std::move(args));
  call->callee->type = type;
  call->target = BuiltinCall{.builtin = Builtin::DictNew};
  call->type = type;

  return call;
}

}
