#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

EventTargetChecker::EventTargetChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->eventMaps = &all.getEventMaps();
  this->typeParams = &all.getTypeParams();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->libraryCalls = &all.getLibraryCalls();
  this->typeDeclarer = &all.getTypeDeclarer();
  this->typeResolver = &all.getTypeResolver();
  this->typeRelations = &all.getTypeRelations();
  this->closureChecker = &all.getClosureChecker();
  this->emitterChecker = &all.getEmitterChecker();
  this->returnInferrer = &all.getReturnInferrer();
  this->argumentChecker = &all.getArgumentChecker();
  this->genericInstantiator = &all.getGenericInstantiator();
}

EventTargetChecker::ResultListenerEvent
EventTargetChecker::listenerEvent(const Type *type, const ClassDecl *eventClass) {
  const ClassDecl *declared = ClassDeclarer::classOf(type);
  if (!declared || !declared->derivesFrom(eventClass) || type->isOptional()) {
    std::string message = ErrorTextEventTarget::listener(type->name());

    return err(EventTargetCheckerListenerEventError{
        .code = EventTargetCheckerListenerEventErrorCode::NOT_AN_EVENT,
        .message = message,
    });
  }

  return ok(type);
}

EventTargetChecker::ResultOneErrorValue EventTargetChecker::oneErrorValue(const Type *errors,
                                                                          llvm::StringRef fn) {
  if (!errors->isTuple() || errors->fields().size() != 1) {
    std::string message = ErrorTextEventsFunction::errorValue(fn, errors->name());

    return err(EventTargetCheckerOneErrorValueError{
        .code = EventTargetCheckerOneErrorValueErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }

  return ok(errors->fields().front().type);
}

EventTargetChecker::ResultEmitterEvents
EventTargetChecker::emitterEvents(const Type *type, llvm::StringRef fn, bool target) {
  const Type *events = this->eventMaps->eventMapOf(type);
  if (!events) {
    std::string message = ErrorTextEventsFunction::notEmitter(fn, target, type->name());

    return err(EventTargetCheckerEmitterEventsError{
        .code = EventTargetCheckerEmitterEventsErrorCode::NOT_AN_EMITTER,
        .message = message,
    });
  }

  return ok(events);
}

Expr *EventTargetChecker::checkEventTargetCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  bool add = member->name == "addEventListener";
  if (!add && member->name != "removeEventListener") {
    return nullptr;
  }
  ClassDecl *eventClass = this->state.getLibraryDeclarations().findLibraryClass("Event");
  TypeAliasDecl *objectListener =
      this->state.getLibraryDeclarations().findLibraryType("EventListenerObject");
  if (!eventClass || !objectListener) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 2, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  const Type *event = eventClass->type;
  const Type *object = this->typeDeclarer->resolveAlias(objectListener);
  auto eventKind = [&](const Type *type, llvm::SMLoc loc) -> const Type * {
    if (type->isError()) {
      return nullptr;
    }
    ResultListenerEvent listened = EventTargetChecker::listenerEvent(type, eventClass);
    if (listened.isErr()) {
      this->state.getDiagnostics().error(loc, listened.error().message);
      return nullptr;
    }

    return type;
  };
  Expr *&listener = call->args[1];
  const Type *kind = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(listener)) {
    kind = event;
    if (!fe->fn->params.empty() && fe->fn->params[0].type && !fe->fn->params[0].rest()) {
      kind = eventKind(this->typeResolver->resolveType(*fe->fn->params[0].type, false),
                       fe->fn->params[0].loc);
    }
    if (!kind) {
      listener = this->exprChecker->checkExpr(listener);
      return call;
    }
    Signature sig;
    sig.params = {kind};
    sig.minArgs = 1;
    sig.ret = Type::voidTy();
    sig.voidResult = true;
    sig.asyncVoid = true;
    listener = this->closureChecker->checkFunctionExpr(fe, &sig);
  } else if (isa<ObjectLitExpr>(listener)) {
    listener = this->argumentChecker->checkArg(listener, object);
  } else {
    listener = this->exprChecker->checkExpr(listener);
    const Type *type = listener->type;
    if (type->isFunction()) {
      kind = type->params().empty() ? event : eventKind(type->params()[0], listener->loc());
      if (kind && type->params().empty()) {
        listener = this->typeRelations->coerce(listener,
                                               Type::function({event}, 1, false, Type::voidTy()));
      } else if (kind) {
        listener =
            this->typeRelations->coerce(listener, Type::function({kind}, 1, false, Type::voidTy()));
      }
    } else {
      listener = this->typeRelations->coerce(listener, object, "the listener");
    }
  }
  if (listener->type->isError()) {
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = given[1] = true;
  if (!add) {
    listener = this->typeRelations->convert(listener, Type::unknown());
    this->state.getInventoryStore().markComparesFunctions();
    return this->emitterChecker->callLibraryMethod(call, member, cls, "removeListenerWith", {},
                                                   &given, false);
  }
  if (kind) {
    return this->emitterChecker->callLibraryMethod(call, member, cls, "addFunctionListener", {kind},
                                                   &given, false);
  }

  return this->emitterChecker->callLibraryMethod(call, member, cls, "addObjectListener", {}, &given,
                                                 false);
}

Expr *EventTargetChecker::callLibraryFunction(CallExpr *call, llvm::StringRef name,
                                              llvm::ArrayRef<const Type *> bound,
                                              const std::vector<bool> &given, llvm::SMLoc loc) {
  FunctionDecl *fn = this->state.getLibraryDeclarations().findLibraryFunction(name);
  assert(fn && fn->sym && "a function of the emitter's code");

  return this->callLibrarySymbol(call, fn->sym, bound, given, loc);
}

Expr *EventTargetChecker::callLibraryStatic(CallExpr *call, ClassDecl *cls, llvm::StringRef name,
                                            llvm::ArrayRef<const Type *> bound,
                                            const std::vector<bool> &given, llvm::SMLoc loc) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, true);
  assert(!methods.empty() && methods[0]->sym && "a static method of the library");

  return this->callLibrarySymbol(call, methods[0]->sym, bound, given, loc);
}

Expr *EventTargetChecker::callLibrarySymbol(CallExpr *call, FuncSymbol *sym,
                                            llvm::ArrayRef<const Type *> bound,
                                            const std::vector<bool> &given, llvm::SMLoc loc) {
  this->returnInferrer->resolveParams(sym);
  const Type *ret = sym->getReturnType();
  FuncSymbol *callee = sym;
  if (sym->isGeneric() && !this->state.getFunctionStack().isInsideTrial() &&
      llvm::none_of(bound, [](const Type *t) { return TypeQueries::mentionsParam(t); })) {
    if (FuncSymbol *instance = this->genericInstantiator->instantiate(sym, bound, loc)) {
      callee = instance;
    }
  }
  this->returnInferrer->resolveParams(callee);
  call->target = FunctionCall{.func = callee};
  call->type = ret ? this->typeParams->substitute(ret, sym->typeParams, bound) : Type::error();
  std::vector<const Type *> params = callee->params;
  if (callee == sym) {
    for (const Type *&p : params) {
      p = this->typeParams->substitute(p, sym->typeParams, bound);
    }
  }

  return this->argumentChecker->checkArguments(call, params, callee->minArgs, callee->hasRest, loc,
                                               &given);
}

Expr *EventTargetChecker::checkEventsFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  if (this->libraryCalls->inLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = this->exprChecker->checkExpr(call->args[i]);
    }
  };
  if (fn == "getEventListeners" || fn == "listenerCount") {
    bool rejected = this->callChecker->rejectSpread(call);
    ArgumentChecker::ResultArityMatches arity =
        rejected ? ok(call) : this->argumentChecker->checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
    }
    if (rejected || arity.isErr()) {
      return call;
    }
    Expr *object = this->exprChecker->checkExpr(call->args[0]);
    if (ClassDecl *target = this->state.getLibraryDeclarations().findLibraryClass("EventTarget");
        fn == "getEventListeners" && target && ClassDeclarer::classOf(object->type) &&
        ClassDeclarer::classOf(object->type)->derivesFrom(target)) {
      auto *member = this->state.getAst().make<MemberExpr>(call->loc(), object, "listenersOf");
      auto *method = this->state.getAst().make<CallExpr>(call->loc(), member,
                                                         std::vector<Expr *>{call->args[1]});
      method->type = Type::error();
      std::vector<bool> given{false};
      this->emitterChecker->callLibraryMethod(method, member, ClassDeclarer::classOf(object->type),
                                              "listenersOf", {}, &given, false);
      return method;
    }
    if (object->type->isError()) {
      checkArgsLoosely(1);
      return call;
    }
    ResultEmitterEvents emitter = this->emitterEvents(object->type, fn, fn == "getEventListeners");
    if (emitter.isErr()) {
      this->state.getDiagnostics().error(object->loc(), emitter.error().message);
      checkArgsLoosely(1);
      return call;
    }
    auto *member = this->state.getAst().make<MemberExpr>(
        call->loc(), object, fn == "getEventListeners" ? "listeners" : "listenerCount");
    member->type = Type::error();
    auto *method = this->state.getAst().make<CallExpr>(call->loc(), member,
                                                       std::vector<Expr *>{call->args[1]});
    Expr *checked = this->emitterChecker->checkEmitterCall(
        method, member, ClassDeclarer::classOf(object->type), false);
    return checked ? checked : method;
  }
  if (fn != "once" && fn != "on") {
    return nullptr;
  }
  unsigned most = 3;
  bool timed = false;
  if (call->args.size() == 3) {
    if (auto *options = dyn_cast<ObjectLitExpr>(call->args[2])) {
      timed = llvm::any_of(options->props, [](const ObjectLitExpr::Property &p) {
        return !p.spread() && p.name == "timeout";
      });
    }
  }
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 2, most);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    checkArgsLoosely(1);
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = given[1] = true;
  if (ClassDecl *target = this->state.getLibraryDeclarations().findLibraryClass("EventTarget");
      target && ClassDeclarer::classOf(type) && ClassDeclarer::classOf(type)->derivesFrom(target)) {
    if (fn == "on") {
      std::string message = ErrorTextEventsFunction::targetOn();
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
      checkArgsLoosely(1);
      return call;
    }
    call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::string());

    return this->callLibraryFunction(call, "onceTarget", {}, given, loc);
  }
  ResultEmitterEvents emitter = this->emitterEvents(type, fn, fn == "once");
  if (emitter.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), emitter.error().message);
    checkArgsLoosely(1);
    return call;
  }
  const Type *events = emitter.value();
  const Type *args = this->eventMaps->eventArgs(events, call->args[1], fn);
  if (!args) {
    checkArgsLoosely(2);
    return call;
  }
  const Type *errors = events->isRecord()       ? events->valueType()
                       : events->field("error") ? events->field("error")->type
                                                : nullptr;
  const ClassDecl *source = ClassDeclarer::classOf(type);
  const ClassDecl *root = source && source->generic() ? source->generic() : source;
  bool ends =
      root && root->library() && root->name != "EventEmitter" && root->name != "EventEmitterBase";
  bool runsOnce = (!this->state.getFunctionStack().getCurrentDecl() &&
                   !this->state.getFunctionStack().isInsideAwaitLoop()) ||
                  this->emitterChecker->ownsEmitter(call->args[0]);
  if (!ends && !timed && !runsOnce) {
    llvm::StringRef name =
        isa<NameExpr>(call->args[0]) ? cast<NameExpr>(call->args[0])->name : llvm::StringRef();
    std::string message = ErrorTextEventsFunction::endless(name, fn);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    checkArgsLoosely(2);
    return call;
  }
  if (fn == "once" && call->args.size() == 3 && !timed) {
    std::string message = ErrorTextEventsFunction::options();
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    checkArgsLoosely(2);
    return call;
  }
  if (timed) {
    const Type *failure = Type::never();
    if (errors) {
      this->state.getEventStore().markErrorListened(events);
      ResultOneErrorValue value = EventTargetChecker::oneErrorValue(errors, fn);
      if (value.isErr()) {
        this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
        return call;
      }
      failure = value.value();
    }
    call->args.push_back(this->exprChecker->checkExpr(
        this->state.getAst().make<BoolLitExpr>(call->loc(), errors != nullptr)));
    given.resize(call->args.size(), false);
    given.back() = true;
    return this->callLibraryFunction(call, fn.str() + "Within", {args, failure}, given, loc);
  }
  if (fn == "once") {
    most = 2;
  }
  while (call->args.size() < most) {
    call->args.push_back(this->state.getAst().make<UndefinedExpr>(call->loc()));
  }
  call->args.push_back(this->exprChecker->checkExpr(
      this->state.getAst().make<BoolLitExpr>(call->loc(), errors != nullptr)));
  given.resize(call->args.size(), false);
  given.back() = true;
  if (!errors) {
    return this->callLibraryFunction(call, fn, {args, Type::tuple({})}, given, loc);
  }
  this->state.getEventStore().markErrorListened(events);
  ResultOneErrorValue value = EventTargetChecker::oneErrorValue(errors, fn);
  if (value.isErr()) {
    this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
    return call;
  }
  std::string variant = fn.str() + "Result";

  return this->callLibraryFunction(call, variant, {args, value.value()}, given, loc);
}

Expr *EventTargetChecker::checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func) {
  if (!func->decl ||
      this->state.getLibraryDeclarations().findLibraryFunction(func->decl->name) != func->decl) {
    return nullptr;
  }

  return this->checkEventsFunction(call, func->decl->name, name->loc());
}

}
