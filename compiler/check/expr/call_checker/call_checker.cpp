#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/async/promise_checker/promise_checker.h"
#include "compiler/check/async/promise_static_checker/promise_static_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/check/classes/inherit_checker/inherit_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/expr/new_checker/new_checker.h"
#include "compiler/check/functions/function_method_checker/function_method_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/objects/dict_methods/dict_methods.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/objects/tuple_checker/tuple_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/check/values/array_methods/array_methods.h"
#include "compiler/check/values/global_function_checker/global_function_checker.h"
#include "compiler/check/values/number_methods/number_methods.h"
#include "compiler/check/values/string_methods/string_methods.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/console/console_checker/console_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/intl/intl_method_checker/intl_method_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/json/json_static_checker/json_static_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/result/result_method_checker/result_method_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/library/tz_modules/http_checker/http_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/library/tz_modules/url_checker/url_checker.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

CallChecker::CallChecker(Checker &owner)
    : checker(owner) {}

Expr *CallChecker::checkCall(CallExpr *call, const Type *expected) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  this->checker.argListChecker().expandListSpreads(call->args, call->spread);
  if (auto *index = dyn_cast<IndexExpr>(call->callee); index && !index->optional) {
    if (auto *key = dyn_cast<MemberExpr>(index->index);
        key && (key->name == "iterator" || key->name == "asyncIterator") &&
        this->isGlobalName(key->object, "Symbol")) {
      call->callee = this->checker.ctx_.make<MemberExpr>(
          index->loc(), index->object,
          key->name == "iterator" ? "[Symbol.iterator]" : "[Symbol.asyncIterator]");
    }
  }

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && name->name == "super") {
    return this->checker.inheritChecker().checkSuperCall(call);
  }
  for (const Feature *feature : this->checker.features_->all()) {
    if (Expr *checked = feature->checkAnyCall(this->checker, call, expected)) {
      return checked;
    }
  }
  if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !call->optional) {
    if (this->isGlobalName(member->object, "Promise")) {
      return this->checker.promiseStaticChecker().checkPromiseStatic(call, member, expected);
    }
    auto rejectTypeArgs = [&] {
      if (member->typeArgs.empty()) {
        return;
      }
      this->checker.diags_.error(member->loc(),
                                 "type arguments can be given only to a generic function");
      member->typeArgs.clear();
    };
    if (auto *object = dyn_cast<NameExpr>(member->object);
        !object || !this->checker.scopes().lookup(object->name).cls()) {
      static const llvm::StringLiteral globals[] = {"String", "Number", "Object", "Iterator",
                                                    "Array",  "Map",    "Symbol"};
      auto named = [&](llvm::StringRef g) { return this->isGlobalName(object, g); };
      if (object && (llvm::any_of(globals, named) ||
                     llvm::any_of(this->checker.features_->all(), [&](const Feature *feature) {
                       return llvm::any_of(feature->typeArgFreeGlobals(), named);
                     }))) {
        rejectTypeArgs();
      }
    }
    if (auto *object = dyn_cast<NameExpr>(member->object); object && object->name == "super") {
      return this->checker.inheritChecker().checkSuperMethodCall(call, member);
    }
    for (const Feature *feature : this->checker.features_->all()) {
      if (Expr *checked = feature->checkStaticCall(this->checker, call, member, expected)) {
        return checked;
      }
    }
    if (this->isGlobalName(member->object, "String") ||
        this->isGlobalName(member->object, "Number")) {
      if (this->rejectSpread(call)) {
        return call;
      }
      NumberMethods::ResultStaticCallMade made =
          this->checker.numberMethods().checkStaticCall(call, member);
      bool reported = made.isErr() &&
                      made.error().code == NumberMethodsCheckStaticCallErrorCode::ALREADY_REPORTED;
      if (made.isErr() && !reported) {
        this->checker.diags_.error(member->loc(), made.error().message);
      }
      return made.isOk() ? made.value() : call;
    }
    if (this->isGlobalName(member->object, "Object")) {
      return this->checker.objectChecker().checkObjectCall(call, member, expected);
    }
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? this->checker.scopes().lookup(object->name).cls() : nullptr) {
      return this->checker.staticChecker().checkClassStaticCall(call, member, cls, expected);
    }
    if (object && object->name == "this") {
      this->checker.thisMember_ = member->name;
    }
    if (this->isGlobalName(member->object, "Iterator")) {
      return this->checker.iteratorMethods().checkIteratorFrom(call, member);
    }
    if (this->isGlobalName(member->object, "Array")) {
      return this->checker.iteratorMethods().checkArrayFrom(call, member);
    }
    if (this->isGlobalName(member->object, "Map")) {
      return this->checker.dictMethods().checkMapStatic(call, member);
    }
    static const llvm::StringLiteral consumers[] = {"toArray", "forEach", "reduce",
                                                    "some",    "every",   "find"};
    if (llvm::is_contained(consumers, member->name)) {
      this->checker.iteratorMethods().markFused(member->object);
    }
    member->object = this->checker.genericChecker().apparent(
        this->checker.exprChecker().checkExpr(member->object));
    this->checker.thisMember_ = {};
    if (member->optional) {
      member->object = this->checker.exprChecker().chainLink(member->object);
    }
    const Type *receiver = member->object->type;
    if (!receiver->isClass() && !receiver->isUnion()) {
      rejectTypeArgs();
    }
    for (const Feature *feature : this->checker.features_->all()) {
      if (Expr *checked = feature->checkMethodCall(this->checker, call, member, expected)) {
        return checked;
      }
    }
    UnknownChecker::ResultMethodChecked unknown =
        this->checker.unknownChecker().checkUnknownMethod(call, member);
    if (unknown.isOk()) {
      return unknown.value();
    }
    switch (unknown.error().code) {
      case UnknownCheckerCheckUnknownMethodErrorCode::NOT_UNKNOWN:
        break;
      case UnknownCheckerCheckUnknownMethodErrorCode::NOT_NARROWED:
        this->checker.diags_.error(member->object->loc(), unknown.error().message);
        return call;
      case UnknownCheckerCheckUnknownMethodErrorCode::RUNTIME_CLASS:
      case UnknownCheckerCheckUnknownMethodErrorCode::CLASS_VALUE:
        this->checker.diags_.error(member->loc(), unknown.error().message);
        return call;
    }
    if (receiver->isFunction() && receiver->hasThis()) {
      FunctionMethodChecker::ResultThisCallMade made =
          this->checker.functionMethodChecker().checkThisCall(call, member);
      if (made.isErr()) {
        this->checker.diags_.error(member->loc(), made.error().message);
        return call;
      }
      return made.value();
    }
    if (receiver->isListParam()) {
      FunctionMethodChecker::ResultParamsJoined joined =
          this->checker.functionMethodChecker().checkListMethod(call, member);
      bool reported =
          joined.isErr() &&
          joined.error().code == FunctionMethodCheckerCheckListMethodErrorCode::ALREADY_REPORTED;
      if (joined.isErr() && !reported) {
        this->checker.diags_.error(member->loc(), joined.error().message);
      }
      return joined.isOk() ? joined.value() : call;
    }
    if (receiver->isPromise()) {
      PromiseChecker::ResultPromiseChained chained =
          this->checker.promiseChecker().checkPromiseMethod(call, member, expected);
      if (chained.isOk()) {
        return chained.value();
      }
      llvm::SMLoc at = member->loc();
      switch (chained.error().code) {
        case PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED:
          return call;
        case PromiseCheckerCheckPromiseMethodErrorCode::THEN_TWO_FUNCTIONS:
          at = call->args[1]->loc();
          break;
        case PromiseCheckerCheckPromiseMethodErrorCode::RESULT_METHOD:
        case PromiseCheckerCheckPromiseMethodErrorCode::NO_CATCH:
        case PromiseCheckerCheckPromiseMethodErrorCode::NO_FINALLY:
        case PromiseCheckerCheckPromiseMethodErrorCode::WRONG_ARITY:
        case PromiseCheckerCheckPromiseMethodErrorCode::NO_SUCH_METHOD:
          break;
      }
      this->checker.diags_.error(at, chained.error().message);
      return call;
    }
    if (receiver->isTimer()) {
      TimerChecker::ResultTimerMethodRun ran =
          this->checker.timerChecker().checkTimerMethod(call, member);
      bool reported = ran.isErr() &&
                      ran.error().code == TimerCheckerCheckTimerMethodErrorCode::ALREADY_REPORTED;
      if (ran.isErr() && !reported) {
        this->checker.diags_.error(member->loc(), ran.error().message);
      }
      return ran.isOk() ? ran.value() : call;
    }
    for (const Feature *feature : this->checker.features_->all()) {
      if (Expr *values = feature->methodReceiver(this->checker, member->object)) {
        member->object = values;
        receiver = values->type;
        break;
      }
    }
    if (receiver->isUnion()) {
      const Type *common = this->checker.unionChecker().commonBaseWith(receiver, member->name);
      if (!common) {
        rejectTypeArgs();
        return this->checker.unionChecker().checkUnionMethodCall(call, member);
      }
      member->object = this->checker.typeRelations().convert(member->object, common);
      receiver = common;
    }
    for (const Feature *feature : this->checker.features_->all()) {
      if (Expr *checked = feature->checkClassMethodCall(this->checker, call, member, expected)) {
        return checked;
      }
    }
    if (receiver->isClass()) {
      bool proven = this->checker.urlChecker().checkUrlSetter(member, call);
      Expr *checked = this->checker.methodCallChecker().checkMethodCall(call, member, expected);
      this->checker.httpChecker().checkHeadersCall(call, member);
      if (proven && checked->type->isResult()) {
        auto *dropped = this->checker.ctx_.make<UnaryExpr>(call->loc(), UnaryOp::Void, checked);
        dropped->type = Type::undefinedTy();
        return dropped;
      }
      return checked;
    }
    rejectTypeArgs();
    if (receiver->isTuple()) {
      static const char *const inPlace[] = {"push", "pop",     "shift", "unshift",    "splice",
                                            "sort", "reverse", "fill",  "copyWithin", "join"};
      Expr *copy = llvm::is_contained(inPlace, member->name)
                       ? nullptr
                       : this->checker.tupleChecker().arrayCopy(member->object);
      if (!copy) {
        TupleChecker::ResultTupleMethod method =
            this->checker.tupleChecker().checkTupleMethod(call, member);
        bool reported =
            method.isErr() &&
            method.error().code == TupleCheckerCheckTupleMethodErrorCode::ALREADY_REPORTED;
        if (method.isErr() && !reported) {
          this->checker.diags_.error(member->loc(), method.error().message);
        }
        return method.isOk() ? method.value() : call;
      }
      member->object = copy;
      receiver = copy->type;
    }
    if (receiver->isObject()) {
      return this->checker.objectChecker().checkFieldCall(call, member);
    }
    if (!(receiver->isArray() && member->name == "push") && this->rejectSpread(call)) {
      return call;
    }
    for (const Feature *feature : this->checker.features_->all()) {
      if (Expr *checked = feature->checkValueMethodCall(this->checker, call, member, expected)) {
        return checked;
      }
    }
    if (receiver->isIterator()) {
      return this->checker.iteratorMethods().checkIteratorMethod(call, member);
    }
    if (receiver->isDict()) {
      return this->checker.dictMethods().checkDictMethod(call, member);
    }
    if (receiver->isString()) {
      return this->checker.stringMethods().checkStringMethod(call, member);
    }
    if (receiver->isArray()) {
      return this->checker.arrayMethods().checkArrayMethod(call, member, expected);
    }
    if (receiver->isNumeric()) {
      NumberMethods::ResultMethodCallMade made =
          this->checker.numberMethods().checkNumberMethod(call, member);
      if (made.isErr()) {
        this->checker.diags_.error(member->loc(), made.error().message);
        return call;
      }
      return made.value();
    }
    if (receiver->isBool() && member->name == "toString" && call->args.empty()) {
      call->args.push_back(this->checker.concatChecker().toStringOperand(member->object));
      call->target = BuiltinCall{.builtin = Builtin::StringOf};
      call->type = Type::string();
      return call;
    }
    if (receiver->isOptional()) {
      this->checker.diags_.error(member->object->loc(),
                                 "the value may be undefined; check it before calling '" +
                                     member->name + "'");
    } else if (receiver->isFunction() &&
               (member->name == "call" || member->name == "apply" || member->name == "bind")) {
      this->checker.diags_.error(member->loc(), "'" + member->name +
                                                    "' is not supported: it sets 'this', and a "
                                                    "method taken as a value is bound to its "
                                                    "object already; call the function "
                                                    "directly");
    } else if (!receiver->isError()) {
      this->checker.diags_.error(member->loc(), "property '" + member->name +
                                                    "' does not exist on type '" +
                                                    receiver->name() + "'");
    }
    checkArgsLoosely();
    return call;
  }

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && !call->optional) {
    for (const Feature *feature : this->checker.features_->all()) {
      if (Expr *checked = feature->checkFunctionCall(this->checker, call, name, expected)) {
        return checked;
      }
    }
    TimerChecker::ResultTimerScheduled task =
        this->checker.timerChecker().checkAsyncGlobal(call, name);
    if (task.isOk()) {
      return task.value();
    }
    switch (task.error().code) {
      case TimerCheckerCheckAsyncGlobalErrorCode::NOT_A_TIMER:
        break;
      case TimerCheckerCheckAsyncGlobalErrorCode::ALREADY_REPORTED:
        return call;
      case TimerCheckerCheckAsyncGlobalErrorCode::TIMEOUT_CALLBACK_ARGUMENTS:
        this->checker.diags_.error(call->args[2]->loc(), task.error().message);
        return call;
      case TimerCheckerCheckAsyncGlobalErrorCode::IMMEDIATE_CALLBACK_ARGUMENTS:
        this->checker.diags_.error(call->args[1]->loc(), task.error().message);
        return call;
      case TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY:
        this->checker.diags_.error(name->loc(), task.error().message);
        return call;
    }
    GlobalFunctionChecker::ResultGlobalCallMade global =
        this->checker.globalFunctionChecker().checkGlobalCall(call, name);
    if (global.isOk()) {
      return global.value();
    }
    switch (global.error().code) {
      case GlobalFunctionCheckerCheckGlobalCallErrorCode::NOT_A_GLOBAL:
        break;
      case GlobalFunctionCheckerCheckGlobalCallErrorCode::ALREADY_REPORTED:
        return call;
      case GlobalFunctionCheckerCheckGlobalCallErrorCode::CALL_FAILED:
        this->checker.diags_.error(name->loc(), global.error().message);
        return call;
      case GlobalFunctionCheckerCheckGlobalCallErrorCode::ARGUMENT_FAILED:
        this->checker.diags_.error(call->args[0]->loc(), global.error().message);
        return call;
    }
    ScopeEntry entry = this->checker.topLevelChecker().lookupValue(name->name);
    if (entry.func() && !entry.var()) {
      for (const Feature *feature : this->checker.features_->all()) {
        if (Expr *checked = feature->checkLibraryCall(this->checker, call, name, entry.func())) {
          return checked;
        }
      }
    }
    if (entry.func() && !entry.var()) {
      Expr *checked = this->checkDirectCall(call, entry.func(), name->loc(), expected);
      this->checker.tzModulesChecker().noteHttpsCall(call, entry.func()->decl);
      return checked;
    }
    VarSymbol *var = entry.var();
    FunctionDecl *known = var ? (var->function() ? var->function()
                                 : var->isConst  ? var->lambda()
                                                 : nullptr)
                              : nullptr;
    if (known && known->sym) {
      name->binding = NamedVar{.var = var};
      this->checker.nestedFunctions().noteUse(var, name->loc());
      name->type = this->checker.scopes().varType(var);
      return this->checkDirectCall(call, known->sym, name->loc());
    }
    if (!entry.var() && !entry.func() && !entry.cls() && name->name == "Array" &&
        !call->hasSpread()) {
      auto *made =
          this->checker.ctx_.make<NewExpr>(call->loc(), name->name, name->typeArgs, call->args);

      return this->checker.newChecker().checkNew(made, expected);
    }
    if (!entry.var() && !entry.func()) {
      if (const char *why = !entry.cls() ? whyUnsupported(name->name) : nullptr) {
        this->checker.diags_.error(name->loc(), "'" + name->name + "' is not supported: " + why);
      } else if (!this->checker.nameChecker().checkName(name)->type->isError()) {
        this->checker.diags_.error(name->loc(), "'" + name->name + "' is not a function");
      }
      checkArgsLoosely();
      return call;
    }
  }

  auto *calleeName = dyn_cast<NameExpr>(call->callee);
  call->callee = this->checker.exprChecker().checkExpr(call->callee);
  if (call->optional) {
    call->callee = this->checker.exprChecker().chainLink(call->callee);
  }
  const Type *type = call->callee->type;
  if (type->isFunction()) {
    Expr *checked = this->checkValueCall(call, type);
    if (calleeName && calleeName->var() && !this->checker.streamCallbacks_.empty()) {
      this->checker.chunkTypes().noteStreamCallback(call, calleeName->var());
    }
    return checked;
  }
  if (type->isOptional() && type->inner()->isFunction()) {
    this->checker.diags_.error(call->callee->loc(),
                               "the function may be undefined; check it first, or call it with "
                               "'?.()', which gives undefined then");
  } else if (calleeName && !type->isError()) {
    this->checker.diags_.error(call->callee->loc(), "'" + calleeName->name +
                                                        "' is not a function; it has type '" +
                                                        type->name() + "'");
  } else if (!type->isError()) {
    this->checker.diags_.error(call->callee->loc(),
                               "a value of type '" + type->name() + "' cannot be called");
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
    return this->checker.typeArgInferrer().checkGenericCall(call, func, loc, expected, typeArgs);
  }
  if (!typeArgs.empty()) {
    this->checker.diags_.error(loc, "'" + func->name +
                                        "' is not a generic function; remove the type arguments");
  }
  this->checker.returnInferrer().resolveParams(func);
  call->target = FunctionCall{.func = func};
  call->type = this->checker.returnInferrer().returnType(func);

  return this->checker.argumentChecker().checkArguments(call, func->params, func->minArgs,
                                                        func->hasRest, loc);
}

Expr *CallChecker::checkValueCall(CallExpr *call, const Type *type) {
  call->type = type->ret();
  if (type->hasThis()) {
    this->checker.diags_.error(call->callee->loc(),
                               "the function takes its 'this' first, which a plain call "
                               "does not pass; call it with '.call(obj, ...)' or "
                               "'.apply(obj, args)'");
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return call;
  }

  return this->checker.argumentChecker().checkArguments(call, type->params(), type->minArgs(),
                                                        type->hasRest(), call->callee->loc());
}

bool CallChecker::rejectSpread(CallExpr *call) {
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (!call->spreads(i)) {
      continue;
    }
    this->checker.diags_.error(call->args[i]->loc(),
                               "a spread argument can only be passed to a rest parameter, "
                               "Math.min, Math.max or push");
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    call->type = Type::error();

    return true;
  }

  return false;
}

bool CallChecker::isGlobalName(const Expr *expr, llvm::StringRef name) const {
  if (auto *m = dyn_cast<MemberExpr>(expr); m && name == "crypto" && m->name == "crypto" &&
                                            !m->optional &&
                                            this->isGlobalName(m->object, "globalThis")) {
    return true;
  }
  auto *n = dyn_cast<NameExpr>(expr);
  if (!n || n->name != name) {
    return false;
  }
  if (this->checker.builtinModuleChecker().nodeName(name)) {
    return false;
  }
  ScopeEntry shadow = this->checker.scopes().lookup(name);

  return !shadow.var() && !shadow.func() && !shadow.cls();
}

}
