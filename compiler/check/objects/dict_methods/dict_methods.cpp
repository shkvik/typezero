#include "compiler/check/objects/dict_methods/dict_methods.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/objects/dict_check_helpers/dict_check_helpers.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/dict_new_checker/dict_new_checker.h"
#include "llvm/ADT/STLExtras.h"


namespace tz {

DictMethods::DictMethods(Checker &owner)
    : checker(owner) {}

Expr *DictMethods::checkDictMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string text = dictText(member->object, type);
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  call->type = Type::error();
  if (type->isRecord()) {
    std::string key = text + "." + name.str();
    if (name == "hasOwnProperty" && call->args.size() == 1) {
      this->checker.diags_.error(
          member->loc(), "a Record inherits nothing from Object.prototype, and '" + key +
                             "' is a key like any other; ask for a key with 'key in " + text + "'");
    } else if (type->valueType()->isFunction()) {
      this->checker.diags_.error(member->loc(),
                                 "'" + key +
                                     "' may be undefined, as a key may be missing; call it with "
                                     "'" +
                                     key + "?.(...)'");
    } else {
      this->checker.diags_.error(member->loc(),
                                 "a Record has no methods, only keys, and '" + key + "' is a '" +
                                     type->valueType()->name() +
                                     " | undefined'; for the keys use 'Object.keys(" + text + ")'");
    }
    checkArgsLoosely();
    return call;
  }
  const Type *key = type->keyType();
  const Type *value = type->valueType();
  bool isMap = type->isMap();
  auto set = [&](Builtin builtin, const Type *result) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return call;
  };
  auto arity = [&](unsigned min, unsigned max) {
    ArgumentChecker::ResultArityMatches matches =
        this->checker.argumentChecker().checkArity(call, min, max);
    if (matches.isErr()) {
      this->checker.diags_.error(member->loc(), matches.error().message);
    }

    return matches.isOk();
  };
  auto keyArg = [&](size_t i) {
    call->args[i] = this->checker.argumentChecker().checkArg(call->args[i], key);
  };

  if (name == "get" && isMap) {
    if (!arity(1, 1)) {
      return call;
    }
    keyArg(0);
    return set(Builtin::MapGet, Type::optional(value));
  }
  if (name == "set" && isMap) {
    if (!arity(2, 2)) {
      return call;
    }
    keyArg(0);
    call->args[1] = this->checker.argumentChecker().checkArg(call->args[1], value);
    return set(Builtin::MapSet, type);
  }
  if (name == "add" && !isMap) {
    if (!arity(1, 1)) {
      return call;
    }
    keyArg(0);
    return set(Builtin::SetAdd, type);
  }
  if (name == "has" || name == "delete") {
    if (!arity(1, 1)) {
      return call;
    }
    keyArg(0);
    return set(name == "has" ? Builtin::MapHas : Builtin::MapDelete, Type::boolTy());
  }
  if (name == "clear") {
    if (!arity(0, 0)) {
      return call;
    }
    return set(Builtin::MapClear, Type::voidTy());
  }
  if (name == "keys" || name == "values" || name == "entries") {
    if (!arity(0, 0)) {
      return call;
    }
    if (!isMap) {
      return name == "entries" ? set(Builtin::SetEntries, Type::iterator(Type::tuple({key, key})))
                               : set(Builtin::SetValues, Type::iterator(key));
    }
    return name == "keys"     ? set(Builtin::MapKeys, Type::iterator(key))
           : name == "values" ? set(Builtin::MapValues, Type::iterator(value))
                              : set(Builtin::MapEntries, Type::iterator(Type::tuple({key, value})));
  }
  if (name == "forEach") {
    if (call->args.size() == 2) {
      this->checker.diags_.error(
          call->args[1]->loc(),
          "the second argument of 'forEach' sets 'this' for the callback, which is not "
          "supported");
      checkArgsLoosely();
      return call;
    }
    if (!arity(1, 1)) {
      return call;
    }
    Checker::Signature sig;
    sig.pad = false;
    sig.voidResult = true;
    sig.params = {isMap ? value : key, key, type};
    call->args[0] = this->checker.callbackChecker().checkFunctionArg(call->args[0], sig,
                                                                     isMap ? 2 : 1, "forEach");
    this->checker.flowChecker().calledFunction();
    return set(Builtin::MapForEach, Type::voidTy());
  }
  struct Combine {
    llvm::StringLiteral name;
    Builtin builtin;
    bool test;
  };
  static const Combine combines[] = {
      {"union", Builtin::SetUnion, false},
      {"intersection", Builtin::SetIntersection, false},
      {"difference", Builtin::SetDifference, false},
      {"symmetricDifference", Builtin::SetSymmetricDifference, false},
      {"isSubsetOf", Builtin::SetIsSubsetOf, true},
      {"isSupersetOf", Builtin::SetIsSupersetOf, true},
      {"isDisjointFrom", Builtin::SetIsDisjointFrom, true},
  };
  for (const Combine &c : combines) {
    if (isMap || c.name != name) {
      continue;
    }
    if (!arity(1, 1)) {
      return call;
    }
    Expr *&other = call->args[0];
    other = this->checker.exprChecker().checkExpr(other, type);
    const Type *otherType = other->type;
    if (otherType->isError()) {
      return call;
    }
    if (!otherType->isDict() || otherType->isRecord() || otherType->keyType() != key) {
      this->checker.diags_.error(other->loc(), "'" + name + "' needs a 'Set<" + key->name() +
                                                   ">' or a 'Map<" + key->name() +
                                                   ", ...>', found '" + otherType->name() +
                                                   "'; make a Set first, e.g. 'new Set(xs)'");
      return call;
    }
    return set(c.builtin, c.test ? Type::boolTy() : type);
  }
  std::string message =
      "property '" + name.str() + "' does not exist on type '" + type->name().str() + "'";
  if (name == "length") {
    message += "; the number of entries is '" + text + ".size'";
  } else if (isMap && name == "add") {
    message += "; a Map sets an entry with '" + text + ".set(key, value)'";
  } else if (!isMap && (name == "set" || name == "get")) {
    message += "; a Set adds a value with '" + text + ".add(value)' and checks one with '" + text +
               ".has(value)'";
  } else if (name == "map" || name == "filter" || name == "reduce" || name == "some" ||
             name == "every" || name == "find") {
    message += "; call it on an iterator, e.g. '" + text + (isMap ? ".entries()" : ".values()") +
               "." + name.str() + "(...)', or on an array, '[..." + text + "]." + name.str() +
               "(...)'";
  }
  this->checker.diags_.error(member->loc(), message);
  checkArgsLoosely();

  return call;
}

Expr *DictMethods::checkFromEntries(CallExpr *call, MemberExpr *member, const Type *expected) {
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  const Type *type = wanted && wanted->isRecord() ? wanted : nullptr;
  const Type *element = type ? Type::tuple({Type::string(), type->valueType()}) : nullptr;
  const Type *key = nullptr;
  const Type *value = nullptr;
  if (!type && this->checker.dictNewChecker().inferPairs(call->args[0], key, value)) {
    if (key->isError()) {
      return call;
    }
    element = call->args[0]->type->inner();
  } else {
    call->args[0] = this->checker.dictNewChecker().checkDictSource(call->args[0], true, element,
                                                                   "'Object.fromEntries'");
  }
  if (element->isError()) {
    return call;
  }
  if (!type) {
    if (!element->fields()[0].type->isString()) {
      this->checker.diags_.error(
          call->args[0]->loc(),
          "the keys of an object are strings, and JavaScript would turn '" +
              element->fields()[0].type->name() +
              "' keys into them; convert them first, or make a Map: 'new Map(...)'");
      return call;
    }
    const Type *values = element->fields()[1].type;
    if (values->isUndefined()) {
      this->checker.diags_.error(call->args[0]->loc(),
                                 "cannot infer the type of the values from 'undefined'; "
                                 "declare it, e.g. 'const r: Record<string, int | "
                                 "undefined> = Object.fromEntries(...)'");
      return call;
    }
    type = Type::dict(Type::DictKind::Record, Type::string(), values);
  }
  call->target = BuiltinCall{.builtin = Builtin::DictNew};
  call->type = type;

  return call;
}

Expr *DictMethods::checkGroupBy(CallExpr *call, MemberExpr *member, bool map) {
  std::string fn = map ? "Map.groupBy" : "Object.groupBy";
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  const Type *element = nullptr;
  call->args[0] =
      this->checker.dictNewChecker().checkDictSource(call->args[0], false, element, "'" + fn + "'");
  if (element->isError()) {
    call->args[1] = this->checker.exprChecker().checkExpr(call->args[1]);
    return call;
  }
  Checker::Signature sig;
  sig.pad = false;
  sig.params = {element, Type::intTy()};
  call->args[1] = this->checker.callbackChecker().checkFunctionArg(call->args[1], sig, 1, fn);
  this->checker.flowChecker().calledFunction();
  const Type *callback = call->args[1]->type;
  if (!callback->isFunction()) {
    return call;
  }
  const Type *key = callback->ret();
  if (key->isVoid() || key->isUndefined()) {
    this->checker.diags_.error(call->args[1]->loc(),
                               "the callback of '" + fn + "' must return the key of a group");
    return call;
  }
  if (!map && !key->isString()) {
    this->checker.diags_.error(
        call->args[1]->loc(),
        "the keys of an object are strings, and JavaScript would turn '" + key->name() +
            "' keys into them; return a string, or group into a Map: 'Map.groupBy(...)'");
    return call;
  }
  if (!this->checker.dictChecker().checkKeyType(key, call->args[1]->loc())) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::DictGroupBy};
  call->type =
      Type::dict(map ? Type::DictKind::Map : Type::DictKind::Record, key, Type::array(element));

  return call;
}

Expr *DictMethods::checkMapStatic(CallExpr *call, MemberExpr *member) {
  if (member->name == "groupBy") {
    return this->checkGroupBy(call, member, true);
  }
  this->checker.diags_.error(member->loc(), "'Map." + member->name +
                                                "' is not supported; of Map only "
                                                "'Map.groupBy' and 'new Map' are");
  for (Expr *&arg : call->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
  }

  return call;
}

Expr *DictMethods::checkDictObjectCall(CallExpr *call, llvm::StringRef fn) {
  Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (!type->isRecord()) {
    std::string text = dictText(arg, type);
    this->checker.diags_.error(
        arg->loc(), "'Object." + fn + "' of a " + (type->isMap() ? "Map" : "Set") +
                        " is always empty in JavaScript: its entries are no properties; "
                        "use '[..." +
                        text + "." + (fn == "entries" && type->isSet() ? "values" : fn) + "()]'");
    return call;
  }
  const Type *value = type->valueType();
  call->target = BuiltinCall{.builtin = fn == "keys"     ? Builtin::ObjectKeys
                                        : fn == "values" ? Builtin::ObjectValues
                                                         : Builtin::ObjectEntries};
  call->type = fn == "keys"     ? Type::array(Type::string())
               : fn == "values" ? Type::array(value)
                                : Type::array(Type::tuple({Type::string(), value}));

  return call;
}

}
