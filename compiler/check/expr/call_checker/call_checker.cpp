#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/library/console/console_checker/console_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/async/promise_checker/promise_checker.h"
#include "compiler/check/async/promise_static_checker/promise_static_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/library/result/result_async_checker/result_async_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/check/values/global_function_checker/global_function_checker.h"
#include "compiler/check/values/number_methods/number_methods.h"
#include "compiler/check/values/string_methods/string_methods.h"
#include "compiler/check/values/array_methods/array_methods.h"
#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/functions/function_method_checker/function_method_checker.h"
#include "compiler/check/objects/dict_methods/dict_methods.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/inherit_checker/inherit_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/objects/tuple_checker/tuple_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/intl/intl_method_checker/intl_method_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/json/json_static_checker/json_static_checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/result/result_method_checker/result_method_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

CallChecker::CallChecker(Sema &sema) : sema_(sema) {}

Expr *CallChecker::checkCall(CallExpr *call, const Type *expected) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
  };
  // `...args` of an argument list: its parameters.
  sema_.argListChecker().expandListSpreads(call->args, call->spread);
  // `x[Symbol.iterator]()`: the method of that name (docs/iterators.md,
  // section 5).
  if (auto *index = dyn_cast<IndexExpr>(call->callee); index && !index->optional) {
    if (auto *key = dyn_cast<MemberExpr>(index->index);
        key && (key->name == "iterator" || key->name == "asyncIterator") &&
        isGlobalName(key->object, "Symbol")) {
      call->callee = sema_.ctx_.make<MemberExpr>(index->loc(), index->object,
                                           key->name == "iterator" ? "[Symbol.iterator]"
                                                                   : "[Symbol.asyncIterator]");
    }
  }

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && name->name == "super") {
    return sema_.inheritChecker().checkSuperCall(call);
  }
  // A call a topic of the library takes before the language looks at it: the functions of
  // node's modules by their imported names.
  for (const Feature *feature : sema_.features_->all()) {
    if (Expr *checked = feature->checkAnyCall(sema_, call, expected)) {
      return checked;
    }
  }
  if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !call->optional) {
    if (isGlobalName(member->object, "Promise")) { // Promise.withResolvers<T>() too
      return sema_.promiseStaticChecker().checkPromiseStatic(call, member, expected);
    }
    // Type arguments, for a generic method of a class; those of anything else
    // are reported once the receiver is known.
    auto rejectTypeArgs = [&] {
      if (member->typeArgs.empty()) {
        return;
      }
      sema_.diags_.error(member->loc(), "type arguments can be given only to a generic function and to "
                                  "'JSON.parse<T>'");
      member->typeArgs.clear();
    };
    if (auto *object = dyn_cast<NameExpr>(member->object); !object || !sema_.scopes().lookup(object->name).cls) {
      static const llvm::StringLiteral globals[] = {"String", "Number", "Object", "Iterator",
                                                    "Array",  "Map",    "Symbol"};
      auto named = [&](llvm::StringRef g) { return isGlobalName(object, g); };
      if (object && (llvm::any_of(globals, named) ||
                     llvm::any_of(sema_.features_->all(), [&](const Feature *feature) {
                       return llvm::any_of(feature->typeArgFreeGlobals(), named);
                     }))) {
        rejectTypeArgs();
      }
    }
    if (auto *object = dyn_cast<NameExpr>(member->object); object && object->name == "super") {
      return sema_.inheritChecker().checkSuperMethodCall(call, member);
    }
    // `JSON.parse(...)`, `Math.max(...)`, `Buffer.from(...)`: a function of a topic of the
    // library.
    for (const Feature *feature : sema_.features_->all()) {
      if (Expr *checked = feature->checkStaticCall(sema_, call, member, expected)) {
        return checked;
      }
    }
    if (isGlobalName(member->object, "String") || isGlobalName(member->object, "Number")) {
      if (rejectSpread(call)) {
        return call;
      }
      return sema_.numberMethods().checkStaticCall(call, member, expected);
    }
    if (isGlobalName(member->object, "Object")) {
      return sema_.objectChecker().checkObjectCall(call, member, expected);
    }
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? sema_.scopes().lookup(object->name).cls : nullptr) {
      return sema_.staticChecker().checkClassStaticCall(call, member, cls, expected);
    }
    if (object && object->name == "this") {
      sema_.thisMember_ = member->name;
    }
    if (isGlobalName(member->object, "Iterator")) {
      return sema_.iteratorMethods().checkIteratorFrom(call, member);
    }
    if (isGlobalName(member->object, "Array")) {
      return sema_.iteratorMethods().checkArrayFrom(call, member);
    }
    if (isGlobalName(member->object, "Map")) {
      return sema_.dictMethods().checkMapStatic(call, member);
    }
    // A consumer of a lazy chain runs it where it is written.
    static const llvm::StringLiteral consumers[] = {"toArray", "forEach", "reduce",
                                                    "some",    "every",   "find"};
    if (llvm::is_contained(consumers, member->name)) {
      sema_.iteratorMethods().markFused(member->object);
    }
    member->object = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(member->object));
    sema_.thisMember_ = {};
    if (member->optional) {
      member->object = sema_.exprChecker().chainLink(member->object);
    }
    const Type *receiver = member->object->type;
    if (!receiver->isClass() && !receiver->isUnion()) {
      rejectTypeArgs();
    }
    // `x.f(...)` of a value of a type of a topic: a Date, a RegExp, a Buffer.
    for (const Feature *feature : sema_.features_->all()) {
      if (Expr *checked = feature->checkMethodCall(sema_, call, member, expected)) {
        return checked;
      }
    }
    if (Expr *checked = sema_.unknownChecker().checkUnknownMethod(call, member)) {
      return checked;
    }
    if (receiver->isFunction() && receiver->hasThis()) {
      return sema_.functionMethodChecker().checkThisCall(call, member);
    }
    if (receiver->isListParam()) {
      return sema_.functionMethodChecker().checkListMethod(call, member);
    }
    if (receiver->isPromise()) {
      return sema_.promiseChecker().checkPromiseMethod(call, member, expected);
    }
    if (receiver->isTimer()) {
      return sema_.timerChecker().checkTimerMethod(call, member);
    }
    // The value whose methods those of the receiver are, as a match's are its captures'.
    for (const Feature *feature : sema_.features_->all()) {
      if (Expr *object = feature->methodReceiver(sema_, member->object)) {
        member->object = object;
        receiver = object->type;
        break;
      }
    }
    // A method of every member of a union: through a type they all are, or
    // that of each.
    if (receiver->isUnion()) {
      const Type *common = sema_.unionChecker().commonBaseWith(receiver, member->name);
      if (!common) {
        rejectTypeArgs();
        return sema_.unionChecker().checkUnionMethodCall(call, member);
      }
      member->object = sema_.typeRelations().convert(member->object, common);
      receiver = common;
    }
    for (const Feature *feature : sema_.features_->all()) {
      if (Expr *checked = feature->checkClassMethodCall(sema_, call, member, expected)) {
        return checked;
      }
    }
    if (receiver->isClass()) {
      return sema_.methodCallChecker().checkMethodCall(call, member, expected);
    }
    rejectTypeArgs();
    if (receiver->isTuple()) {
      return sema_.tupleChecker().checkTupleMethod(call, member);
    }
    if (receiver->isObject()) {
      return sema_.objectChecker().checkFieldCall(call, member);
    }
    if (!(receiver->isArray() && member->name == "push") && rejectSpread(call)) {
      return call;
    }
    // A method a topic gives the values of the language: `s.localeCompare(t)`.
    for (const Feature *feature : sema_.features_->all()) {
      if (Expr *checked = feature->checkValueMethodCall(sema_, call, member, expected)) {
        return checked;
      }
    }
    if (receiver->isIterator()) {
      return sema_.iteratorMethods().checkIteratorMethod(call, member);
    }
    if (receiver->isDict()) {
      return sema_.dictMethods().checkDictMethod(call, member);
    }
    if (receiver->isString()) {
      return sema_.stringMethods().checkStringMethod(call, member);
    }
    if (receiver->isArray()) {
      return sema_.arrayMethods().checkArrayMethod(call, member, expected);
    }
    if (receiver->isNumeric()) {
      return sema_.numberMethods().checkNumberMethod(call, member);
    }
    if (receiver->isBool() && member->name == "toString" && call->args.empty()) {
      // `x.toString()` is `String(x)`.
      call->args.push_back(sema_.concatChecker().toStringOperand(member->object));
      call->builtin = Builtin::StringOf;
      call->type = Type::string();
      return call;
    }
    if (receiver->isOptional()) {
      sema_.diags_.error(member->object->loc(),
                   "the value may be undefined; check it before calling '" + member->name + "'");
    } else if (receiver->isFunction() &&
               (member->name == "call" || member->name == "apply" || member->name == "bind")) {
      sema_.diags_.error(member->loc(), "'" + member->name +
                                      "' is not supported: it sets 'this', and a "
                                      "method taken as a value is bound to its "
                                      "object already; call the function "
                                      "directly");
    } else if (!receiver->isError()) {
      sema_.diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                      receiver->name() + "'");
    }
    checkArgsLoosely();
    return call;
  }

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && !call->optional) {
    // `ok(x)`, `embedJson(...)`: a function of a topic of the library.
    for (const Feature *feature : sema_.features_->all()) {
      if (Expr *checked = feature->checkFunctionCall(sema_, call, name, expected)) {
        return checked;
      }
    }
    if (Expr *task = sema_.timerChecker().checkAsyncGlobal(call, name)) {
      return task;
    }
    if (Expr *builtin = sema_.globalFunctionChecker().checkGlobalCall(call, name, expected)) {
      return builtin;
    }
    Sema::ScopeEntry entry = sema_.topLevelChecker().lookupValue(name->name);
    // once() of node:events, pipeline() of node:stream: a function of a module of a topic
    // written in tz, which the topic checks itself.
    if (entry.func && !entry.var) {
      for (const Feature *feature : sema_.features_->all()) {
        if (Expr *checked = feature->checkLibraryCall(sema_, call, name, entry.func)) {
          return checked;
        }
      }
    }
    if (entry.func && !entry.var) { // a top-level function
      Expr *checked = checkDirectCall(call, entry.func, name->loc(), expected);
      sema_.tzModulesChecker().noteHttpsCall(call, entry.func->decl);
      return checked;
    }
    // A function declared in a block, or a constant arrow: called directly,
    // with the environment its variable holds.
    VarSymbol *var = entry.var;
    FunctionDecl *known = var ? (var->function  ? var->function
                                 : var->isConst ? var->lambda
                                                : nullptr)
                              : nullptr;
    if (known && known->sym) {
      name->var = var;
      sema_.nestedFunctions().noteUse(var, name->loc());
      name->type = sema_.scopes().varType(var);
      return checkDirectCall(call, known->sym, name->loc());
    }
    if (!entry.var && !entry.func) {
      // `TypeError("bad")` makes the error as `new TypeError("bad")` does, as JavaScript has
      // its kinds of Error (not other classes) callable.
      if (entry.cls && entry.cls == sema_.throwChecker().errorKindClass(name->name) && name->typeArgs.empty() &&
          !call->optional && !call->hasSpread()) {
        auto *made =
            sema_.ctx_.make<NewExpr>(call->loc(), name->name, std::vector<TypeRef>{}, call->args);
        return sema_.exprChecker().checkExpr(made, expected);
      }
      // `Function("a", "return a")`, which is a value only as a class.
      if (const char *why = !entry.cls ? whyUnsupported(name->name) : nullptr) {
        sema_.diags_.error(name->loc(), "'" + name->name + "' is not supported: " + why);
      } else if (!sema_.nameChecker().checkName(name)->type->isError()) { // NaN, Infinity
        sema_.diags_.error(name->loc(), "'" + name->name + "' is not a function");
      }
      checkArgsLoosely();
      return call;
    }
  }

  // A call through a function value: `f(x)` with a variable f, `fs[i](x)`,
  // `make()(x)`, `f?.(x)`.
  auto *calleeName = dyn_cast<NameExpr>(call->callee);
  call->callee = sema_.exprChecker().checkExpr(call->callee);
  if (call->optional) {
    call->callee = sema_.exprChecker().chainLink(call->callee);
  }
  const Type *type = call->callee->type;
  if (type->isFunction()) {
    Expr *checked = checkValueCall(call, type);
    // The callback of a Transform's transform() (sema_stream.cpp).
    if (calleeName && calleeName->var && !sema_.streamCallbacks_.empty()) {
      sema_.chunkTypes().noteStreamCallback(call, calleeName->var);
    }
    return checked;
  }
  if (type->isOptional() && type->inner()->isFunction()) {
    sema_.diags_.error(call->callee->loc(),
                 "the function may be undefined; check it first, or call it with "
                 "'?.()', which gives undefined then");
  } else if (calleeName && !type->isError()) {
    sema_.diags_.error(call->callee->loc(), "'" + calleeName->name +
                                          "' is not a function; it has type '" + type->name() +
                                          "'");
  } else if (!type->isError()) {
    sema_.diags_.error(call->callee->loc(), "a value of type '" + type->name() + "' cannot be called");
  }
  checkArgsLoosely();

  return call;
}

Expr *CallChecker::checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc,
                            const Type *expected) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  llvm::ArrayRef<TypeRef> typeArgs =
      name ? llvm::ArrayRef<TypeRef>(name->typeArgs) : llvm::ArrayRef<TypeRef>();
  if (func->isGeneric()) {
    return sema_.typeArgInferrer().checkGenericCall(call, func, loc, expected, typeArgs);
  }
  if (!typeArgs.empty()) {
    sema_.diags_.error(loc, "'" + func->name + "' is not a generic function; remove the type arguments");
  }
  sema_.returnInferrer().resolveParams(func);
  call->func = func;
  call->type = sema_.returnInferrer().returnType(func);

  return sema_.argumentChecker().checkArguments(call, func->params, func->minArgs, func->hasRest, loc);
}

Expr *CallChecker::checkValueCall(CallExpr *call, const Type *type) {
  call->type = type->ret();
  if (type->hasThis()) {
    // `f(x)` of `(this: unknown, x: int) => R` would give it no `this`.
    sema_.diags_.error(call->callee->loc(), "the function takes its 'this' first, which a plain call "
                                      "does not pass; call it with '.call(obj, ...)' or "
                                      "'.apply(obj, args)'");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }

  return sema_.argumentChecker().checkArguments(call, type->params(), type->minArgs(), type->hasRest(),
                        call->callee->loc());
}

bool CallChecker::rejectSpread(CallExpr *call) {
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (!call->spreads(i)) {
      continue;
    }
    sema_.diags_.error(call->args[i]->loc(), "a spread argument can only be passed to a rest parameter, "
                                       "Math.min, Math.max or push");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    call->type = Type::error();

    return true;
  }

  return false;
}

bool CallChecker::isGlobalName(const Expr *expr, llvm::StringRef name) const {
  // `globalThis.crypto` is the global crypto (Web Crypto, sema_webcrypto.cpp).
  if (auto *m = dyn_cast<MemberExpr>(expr); m && name == "crypto" && m->name == "crypto" &&
                                            !m->optional && isGlobalName(m->object, "globalThis")) {
    return true;
  }
  auto *n = dyn_cast<NameExpr>(expr);
  if (!n || n->name != name) {
    return false;
  }
  // A name imported from a built-in module hides the global one too
  // (setTimeout of node:timers/promises).
  if (sema_.builtinModuleChecker().nodeName(name)) {
    return false;
  }
  Sema::ScopeEntry shadow = sema_.scopes().lookup(name);

  return !shadow.var && !shadow.func && !shadow.cls;
}

} // namespace tz
