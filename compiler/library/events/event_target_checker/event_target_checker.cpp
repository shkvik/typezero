#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

EventTargetChecker::EventTargetChecker(Checker &checker) : checker_(checker) {}

Expr *EventTargetChecker::checkEventTargetCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  bool add = member->name == "addEventListener";
  if (!add && member->name != "removeEventListener") {
    return nullptr;
  }
  ClassDecl *eventClass = checker_.libraryClass("Event");
  TypeAliasDecl *objectListener = checker_.libTypes_.lookup("EventListenerObject");
  if (!eventClass || !objectListener) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 2, 3)) {
    return call;
  }
  call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
  const Type *event = eventClass->type;
  const Type *object = checker_.typeDeclarer().resolveAlias(objectListener);
  auto eventKind = [&](const Type *type, llvm::SMLoc loc) -> const Type * {
    if (type->isError()) {
      return nullptr;
    }
    if (ClassDecl *c = ClassDeclarer::classOf(type);
        c && c->derivesFrom(eventClass) && !type->isOptional()) {
      return type;
    }
    checker_.diags_.error(loc,
                          "a listener of an EventTarget takes an Event, or a kind of it such as "
                          "'CustomEvent<int>', but this one takes '" +
                              type->name() + "'");

    return nullptr;
  };
  Expr *&listener = call->args[1];
  const Type *kind = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(listener)) {
    kind = event;
    if (!fe->fn->params.empty() && fe->fn->params[0].type && !fe->fn->params[0].rest) {
      kind = eventKind(checker_.typeResolver().resolveType(*fe->fn->params[0].type, false),
                       fe->fn->params[0].loc);
    }
    if (!kind) {
      listener = checker_.exprChecker().checkExpr(listener);
      return call;
    }
    Checker::Signature sig;
    sig.params = {kind};
    sig.minArgs = 1;
    sig.ret = Type::voidTy();
    sig.voidResult = true;
    sig.asyncVoid = true;
    listener = checker_.closureChecker().checkFunctionExpr(fe, &sig);
  } else if (isa<ObjectLitExpr>(listener)) {
    listener = checker_.argumentChecker().checkArg(listener, object);
  } else {
    listener = checker_.exprChecker().checkExpr(listener);
    const Type *type = listener->type;
    if (type->isFunction()) {
      kind = type->params().empty() ? event : eventKind(type->params()[0], listener->loc());
      if (kind && type->params().empty()) {
        listener = checker_.typeRelations().coerce(
            listener, Type::function({event}, 1, false, Type::voidTy()));
      } else if (kind) {
        listener = checker_.typeRelations().coerce(
            listener, Type::function({kind}, 1, false, Type::voidTy()));
      }
    } else {
      listener = checker_.typeRelations().coerce(listener, object, "the listener");
    }
  }
  if (listener->type->isError()) {
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = given[1] = true;
  if (!add) {
    listener = checker_.typeRelations().convert(listener, Type::unknown());
    checker_.file_->comparesFunctions = true;
    return checker_.emitterChecker().callLibraryMethod(call, member, cls, "removeListenerWith", {},
                                                       &given, false);
  }
  if (kind) {
    return checker_.emitterChecker().callLibraryMethod(call, member, cls, "addFunctionListener",
                                                       {kind}, &given, false);
  }

  return checker_.emitterChecker().callLibraryMethod(call, member, cls, "addObjectListener", {},
                                                     &given, false);
}

Expr *EventTargetChecker::callLibraryFunction(CallExpr *call, llvm::StringRef name,
                                              llvm::ArrayRef<const Type *> bound,
                                              const std::vector<bool> &given, llvm::SMLoc loc) {
  FunctionDecl *fn = checker_.libFunctions_.lookup(name);
  assert(fn && fn->sym && "a function of the emitter's code");

  return callLibrarySymbol(call, fn->sym, bound, given, loc);
}

Expr *EventTargetChecker::callLibraryStatic(CallExpr *call, ClassDecl *cls, llvm::StringRef name,
                                            llvm::ArrayRef<const Type *> bound,
                                            const std::vector<bool> &given, llvm::SMLoc loc) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, true);
  assert(!methods.empty() && methods[0]->sym && "a static method of the library");

  return callLibrarySymbol(call, methods[0]->sym, bound, given, loc);
}

Expr *EventTargetChecker::callLibrarySymbol(CallExpr *call, FuncSymbol *sym,
                                            llvm::ArrayRef<const Type *> bound,
                                            const std::vector<bool> &given, llvm::SMLoc loc) {
  checker_.returnInferrer().resolveParams(sym);
  const Type *ret = checker_.returnInferrer().returnType(sym);
  FuncSymbol *callee = sym;
  if (sym->isGeneric() && !checker_.trial_ &&
      llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); })) {
    if (FuncSymbol *instance = checker_.genericInstantiator().instantiate(sym, bound, loc)) {
      callee = instance;
    }
  }
  checker_.returnInferrer().resolveParams(callee);
  call->func = callee;
  call->type = ret ? checker_.typeParams().substitute(ret, sym->typeParams, bound) : Type::error();
  std::vector<const Type *> params = callee->params;
  if (callee == sym) {
    for (const Type *&p : params) {
      p = checker_.typeParams().substitute(p, sym->typeParams, bound);
    }
  }

  return checker_.argumentChecker().checkArguments(call, params, callee->minArgs, callee->hasRest,
                                                   loc, &given);
}

Expr *EventTargetChecker::checkEventsFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  if (checker_.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = checker_.exprChecker().checkExpr(call->args[i]);
    }
  };
  if (fn == "getEventListeners" || fn == "listenerCount") {
    if (checker_.callChecker().rejectSpread(call) ||
        !checker_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *object = checker_.exprChecker().checkExpr(call->args[0]);
    if (ClassDecl *target = checker_.libraryClass("EventTarget");
        fn == "getEventListeners" && target && ClassDeclarer::classOf(object->type) &&
        ClassDeclarer::classOf(object->type)->derivesFrom(target)) {
      auto *member = checker_.ctx_.make<MemberExpr>(call->loc(), object, "listenersOf");
      auto *method =
          checker_.ctx_.make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
      method->type = Type::error();
      std::vector<bool> given{false};
      checker_.emitterChecker().callLibraryMethod(
          method, member, ClassDeclarer::classOf(object->type), "listenersOf", {}, &given, false);
      return method;
    }
    if (!object->type->isError() && !checker_.eventMaps().eventMapOf(object->type)) {
      checker_.diags_.error(object->loc(),
                            "'" + fn.str() + "' takes an emitter of node:events" +
                                (fn == "getEventListeners" ? " or an EventTarget" : "") +
                                ", but this is '" + object->type->name().str() + "'");
      checkArgsLoosely(1);
      return call;
    }
    if (object->type->isError()) {
      checkArgsLoosely(1);
      return call;
    }
    auto *member = checker_.ctx_.make<MemberExpr>(
        call->loc(), object, fn == "getEventListeners" ? "listeners" : "listenerCount");
    member->type = Type::error();
    auto *method =
        checker_.ctx_.make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
    Expr *checked = checker_.emitterChecker().checkEmitterCall(
        method, member, ClassDeclarer::classOf(object->type), false);
    return checked ? checked : method;
  }
  if (fn != "once" && fn != "on") {
    return nullptr;
  }
  unsigned most = fn == "on" ? 3 : 2;
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, 2, most)) {
    return call;
  }
  call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    checkArgsLoosely(1);
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = given[1] = true;
  if (ClassDecl *target = checker_.libraryClass("EventTarget");
      target && ClassDeclarer::classOf(type) && ClassDeclarer::classOf(type)->derivesFrom(target)) {
    if (fn == "on") {
      checker_.diags_.error(call->args[0]->loc(),
                            "'on' of node:events takes an emitter of node:events; "
                            "the events of an EventTarget are not supported yet: "
                            "listen with 'addEventListener', or wait for one with "
                            "'await once(target, name)'");
      checkArgsLoosely(1);
      return call;
    }
    call->args[1] = checker_.argumentChecker().checkArg(call->args[1], Type::string());

    return callLibraryFunction(call, "onceTarget", {}, given, loc);
  }
  const Type *events = checker_.eventMaps().eventMapOf(type);
  if (!events) {
    checker_.diags_.error(call->args[0]->loc(), "'" + fn.str() +
                                                    "' takes an emitter of node:events" +
                                                    (fn == "once" ? " or an EventTarget" : "") +
                                                    ", but this is '" + type->name().str() + "'");
    checkArgsLoosely(1);
    return call;
  }
  const Type *args = checker_.eventMaps().eventArgs(events, call->args[1], fn);
  if (!args) {
    checkArgsLoosely(2);
    return call;
  }
  const Type *errors = events->isRecord()       ? events->valueType()
                       : events->field("error") ? events->field("error")->type
                                                : nullptr;
  while (call->args.size() < most) {
    call->args.push_back(checker_.ctx_.make<UndefinedExpr>(call->loc()));
  }
  call->args.push_back(checker_.exprChecker().checkExpr(
      checker_.ctx_.make<BoolLitExpr>(call->loc(), errors != nullptr)));
  given.resize(call->args.size(), false);
  given.back() = true;
  if (!errors) {
    return callLibraryFunction(call, fn, {args, Type::tuple({})}, given, loc);
  }
  checker_.errorListenedMaps_.insert(events);
  if (!errors->isTuple() || errors->fields().size() != 1) {
    checker_.diags_.error(call->args[1]->loc(),
                          "'" + fn.str() +
                              "' gives the error of the emitter as the Err, so its 'error' "
                              "event carries one value; this one carries '" +
                              errors->name().str() + "'");
    return call;
  }
  std::string variant = fn.str() + "Result";

  return callLibraryFunction(call, variant, {args, errors->fields().front().type}, given, loc);
}

Expr *EventTargetChecker::checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func) {
  if (!func->decl || checker_.libFunctions_.lookup(func->decl->name) != func->decl) {
    return nullptr;
  }

  return checkEventsFunction(call, func->decl->name, name->loc());
}

}
