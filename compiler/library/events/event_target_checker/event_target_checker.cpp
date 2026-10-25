#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

EventTargetChecker::EventTargetChecker(Sema &sema) : sema_(sema) {}

Expr *EventTargetChecker::onabortAssign(AssignExpr *assign, MemberExpr *member, bool logical) {
  ClassDecl *signal = sema_.libraryClass("AbortSignal");
  if (!signal || sema_.libraryCalls().inLibrary(assign->loc())) {
    return nullptr;
  }
  ClassDecl *cls = ClassDeclarer::classOf(member->object->type);
  if (!cls || !cls->derivesFrom(signal) || member->object->type->isOptional()) {
    return nullptr;
  }
  // What node has as getters only.
  if (member->name == "aborted" || member->name == "reason") {
    sema_.diags_.error(member->loc(), "'" + member->name +
                                    "' of an AbortSignal is read-only; abort it with its "
                                    "controller, 'controller.abort(reason)'");
    assign->type = Type::error();
    return assign;
  }
  if (member->name != "onabort") {
    return nullptr;
  }
  if (assign->op || logical) {
    sema_.diags_.error(assign->loc(), "only '=' can set 'onabort'");
    assign->type = Type::error();
    return assign;
  }
  auto *callee = sema_.ctx_.make<MemberExpr>(member->loc(), member->object, "setOnabort");
  auto *call = sema_.ctx_.make<CallExpr>(assign->loc(), callee, std::vector<Expr *>{assign->value});
  call->type = Type::error();
  std::vector<bool> given{true};

  return sema_.emitterChecker().callLibraryMethod(call, callee, cls, "setOnabort", {}, &given, false);
}

Expr *EventTargetChecker::checkEventTargetCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  bool add = member->name == "addEventListener";
  if ((!add && member->name != "removeEventListener") || sema_.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }
  ClassDecl *eventClass = sema_.libraryClass("Event");
  TypeAliasDecl *objectListener = sema_.libTypes_.lookup("EventListenerObject");
  if (!eventClass || !objectListener) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, member->loc(), 2, 3)) {
    return call;
  }
  call->args[0] = sema_.argumentChecker().checkArg(call->args[0], Type::string());
  const Type *event = eventClass->type;
  const Type *object = sema_.typeDeclarer().resolveAlias(objectListener);
  // The kind of event a function takes: Event, or a class down its chain.
  auto eventKind = [&](const Type *type, llvm::SMLoc loc) -> const Type * {
    if (type->isError()) {
      return nullptr;
    }
    if (ClassDecl *c = ClassDeclarer::classOf(type); c && c->derivesFrom(eventClass) && !type->isOptional()) {
      return type;
    }
    sema_.diags_.error(loc, "a listener of an EventTarget takes an Event, or a kind of it such as "
                      "'CustomEvent<int>', but this one takes '" +
                          type->name() + "'");

    return nullptr;
  };
  Expr *&listener = call->args[1];
  const Type *kind = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(listener)) {
    kind = event;
    if (!fe->fn->params.empty() && fe->fn->params[0].type && !fe->fn->params[0].rest) {
      kind = eventKind(sema_.typeResolver().resolveType(*fe->fn->params[0].type, /*allowVoid=*/false),
                       fe->fn->params[0].loc);
    }
    if (!kind) {
      listener = sema_.exprChecker().checkExpr(listener);
      return call;
    }
    Sema::Signature sig;
    sig.params = {kind};
    sig.minArgs = 1;
    sig.ret = Type::voidTy();
    sig.voidResult = true;
    sig.asyncVoid = true;
    listener = sema_.closureChecker().checkFunctionExpr(fe, &sig);
  } else if (isa<ObjectLitExpr>(listener)) {
    listener = sema_.argumentChecker().checkArg(listener, object);
  } else {
    listener = sema_.exprChecker().checkExpr(listener);
    const Type *type = listener->type;
    if (type->isFunction()) {
      kind = type->params().empty() ? event : eventKind(type->params()[0], listener->loc());
      if (kind && type->params().empty()) {
        listener = sema_.typeRelations().coerce(listener, Type::function({event}, 1, false, Type::voidTy()));
      } else if (kind) {
        listener = sema_.typeRelations().coerce(listener, Type::function({kind}, 1, false, Type::voidTy()));
      }
    } else {
      listener = sema_.typeRelations().coerce(listener, object, "the listener");
    }
  }
  if (listener->type->isError()) {
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = given[1] = true;
  if (!add) {
    listener = sema_.typeRelations().convert(listener, Type::unknown());
    sema_.file_->comparesFunctions = true; // the listener is found by what it is
    return sema_.emitterChecker().callLibraryMethod(call, member, cls, "removeListenerWith", {}, &given, false);
  }
  if (kind) {
    return sema_.emitterChecker().callLibraryMethod(call, member, cls, "addFunctionListener", {kind}, &given, false);
  }

  return sema_.emitterChecker().callLibraryMethod(call, member, cls, "addObjectListener", {}, &given, false);
}

Expr *EventTargetChecker::callLibraryFunction(CallExpr *call, llvm::StringRef name,
                                llvm::ArrayRef<const Type *> bound, const std::vector<bool> &given,
                                llvm::SMLoc loc) {
  FunctionDecl *fn = sema_.libFunctions_.lookup(name);
  assert(fn && fn->sym && "a function of the emitter's code");
  FuncSymbol *sym = fn->sym;
  sema_.returnInferrer().resolveParams(sym);
  // Of a function that does not write it, inferred before its copies are
  // made (on() gives what its async generator does).
  const Type *ret = sema_.returnInferrer().returnType(sym);
  FuncSymbol *callee = sym;
  if (sym->isGeneric() && !sema_.trial_ &&
      llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); })) {
    if (FuncSymbol *instance = sema_.genericInstantiator().instantiate(sym, bound, loc)) {
      callee = instance;
    }
  }
  sema_.returnInferrer().resolveParams(callee);
  call->func = callee;
  call->type = ret ? sema_.typeParams().substitute(ret, sym->typeParams, bound) : Type::error();
  std::vector<const Type *> params = callee->params;
  if (callee == sym) {
    for (const Type *&p : params) {
      p = sema_.typeParams().substitute(p, sym->typeParams, bound);
    }
  }

  return sema_.argumentChecker().checkArguments(call, params, callee->minArgs, callee->hasRest, loc, &given);
}

Expr *EventTargetChecker::checkEventsFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  if (sema_.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = sema_.exprChecker().checkExpr(call->args[i]);
    }
  };
  // The functions that are methods of the emitter they get first.
  if (fn == "getEventListeners" || fn == "listenerCount") {
    if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *object = sema_.exprChecker().checkExpr(call->args[0]);
    if (ClassDecl *target = sema_.libraryClass("EventTarget");
        fn == "getEventListeners" && target && ClassDeclarer::classOf(object->type) &&
        ClassDeclarer::classOf(object->type)->derivesFrom(target)) {
      // The listeners of an EventTarget, of the kinds of events they take.
      auto *member = sema_.ctx_.make<MemberExpr>(call->loc(), object, "listenersOf");
      auto *method = sema_.ctx_.make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
      method->type = Type::error();
      std::vector<bool> given{false};
      sema_.emitterChecker().callLibraryMethod(method, member, ClassDeclarer::classOf(object->type), "listenersOf", {}, &given, false);
      return method;
    }
    if (!object->type->isError() && !sema_.eventMaps().eventMapOf(object->type)) {
      sema_.diags_.error(object->loc(), "'" + fn.str() + "' takes an emitter of node:events" +
                                      (fn == "getEventListeners" ? " or an EventTarget" : "") +
                                      ", but this is '" + object->type->name().str() + "'");
      checkArgsLoosely(1);
      return call;
    }
    if (object->type->isError()) {
      checkArgsLoosely(1);
      return call;
    }
    auto *member = sema_.ctx_.make<MemberExpr>(call->loc(), object,
                                         fn == "getEventListeners" ? "listeners" : "listenerCount");
    member->type = Type::error();
    auto *method = sema_.ctx_.make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
    Expr *checked = sema_.emitterChecker().checkEmitterCall(method, member, ClassDeclarer::classOf(object->type), /*super=*/false);
    return checked ? checked : method;
  }
  if (fn != "once" && fn != "on") {
    return nullptr;
  }
  // once(emitter, name, { signal }): a Promise of the arguments of the next
  // event, and of its 'error' events, if it has them. on(emitter, name, {
  // signal, close }): an async iterator of the arguments of each event, which
  // its 'error' events end.
  if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, loc, 2, 3)) {
    return call;
  }
  call->args[0] = sema_.exprChecker().checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    checkArgsLoosely(1);
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = given[1] = true;
  if (ClassDecl *target = sema_.libraryClass("EventTarget");
      target && ClassDeclarer::classOf(type) && ClassDeclarer::classOf(type)->derivesFrom(target)) {
    if (fn == "on") {
      sema_.diags_.error(call->args[0]->loc(), "'on' of node:events takes an emitter of node:events; "
                                         "the events of an EventTarget are not supported yet: "
                                         "listen with 'addEventListener', or wait for one with "
                                         "'await once(target, name)'");
      checkArgsLoosely(1);
      return call;
    }
    call->args[1] = sema_.argumentChecker().checkArg(call->args[1], Type::string());

    return callLibraryFunction(call, "onceTarget", {}, given, loc);
  }
  const Type *events = sema_.eventMaps().eventMapOf(type);
  if (!events) {
    sema_.diags_.error(call->args[0]->loc(), "'" + fn.str() + "' takes an emitter of node:events" +
                                           (fn == "once" ? " or an EventTarget" : "") +
                                           ", but this is '" + type->name().str() + "'");
    checkArgsLoosely(1);
    return call;
  }
  const Type *args = sema_.eventMaps().eventArgs(events, call->args[1], fn);
  if (!args) {
    checkArgsLoosely(2);
    return call;
  }
  // Its 'error' events reject the Promise.
  const Type *errors = events->isRecord()       ? events->valueType()
                       : events->field("error") ? events->field("error")->type
                                                : nullptr;
  while (call->args.size() < 3) {
    call->args.push_back(sema_.ctx_.make<UndefinedExpr>(call->loc()));
  }
  call->args.push_back(sema_.exprChecker().checkExpr(sema_.ctx_.make<BoolLitExpr>(call->loc(), errors != nullptr)));
  given.resize(call->args.size(), false);
  given.back() = true;

  return callLibraryFunction(call, fn, {args, errors ? errors : Type::tuple({})}, given, loc);
}

Expr *EventTargetChecker::checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func) {
  if (!func->decl || sema_.libFunctions_.lookup(func->decl->name) != func->decl) {
    return nullptr;
  }

  return checkEventsFunction(call, func->decl->name, name->loc());
}

} // namespace tz
