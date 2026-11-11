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
  this->eventMaps = &all.eventMaps;
  this->typeParams = &all.typeParams;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->libraryCalls = &all.libraryCalls;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->emitterChecker = &all.emitterChecker;
  this->argumentChecker = &all.argumentChecker;
  this->genericInstantiator = &all.genericInstantiator;
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
  if (eventClass == nullptr || objectListener == nullptr) {
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
  const Type *object = this->typeDeclarer->resolveAlias(objectListener);
  std::optional<const Type *> kind = this->typedListener(call, eventClass, object);
  if (!kind.has_value() || call->args[1]->type->isError()) {
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = true;
  given[1] = true;
  if (!add) {
    call->args[1] = this->typeRelations->convert(call->args[1], Type::unknown());
    this->state.getInventoryStore().markComparesFunctions();
    return this->emitterChecker->callLibraryMethod(call, member, cls, "removeListenerWith", {},
                                                   &given, false);
  }
  if (*kind != nullptr) {
    return this->emitterChecker->callLibraryMethod(call, member, cls, "addFunctionListener",
                                                   {*kind}, &given, false);
  }

  return this->emitterChecker->callLibraryMethod(call, member, cls, "addObjectListener", {}, &given,
                                                 false);
}

std::optional<const Type *> EventTargetChecker::typedListener(CallExpr *call, ClassDecl *eventClass,
                                                              const Type *object) {
  Expr *&listener = call->args[1];
  auto *function = dyn_cast<FunctionExpr>(listener);
  if (function != nullptr) {
    return this->typedFunctionListener(call, function, eventClass);
  }
  if (isa<ObjectLitExpr>(listener)) {
    listener = this->argumentChecker->checkArg(listener, object);
    return std::make_optional<const Type *>(nullptr);
  }

  return this->typedValueListener(call, eventClass, object);
}

std::optional<const Type *> EventTargetChecker::typedFunctionListener(CallExpr *call,
                                                                      FunctionExpr *function,
                                                                      ClassDecl *eventClass) {
  Expr *&listener = call->args[1];
  const Type *kind = eventClass->type;
  std::vector<Param> &params = function->fn->params;
  bool typedParam = !params.empty() && params[0].type && !params[0].rest();
  if (typedParam) {
    const Type *written = this->typeResolver->resolveType(*params[0].type, false);
    kind = this->listenedEvent(written, params[0].loc, eventClass);
  }
  if (kind == nullptr) {
    listener = this->exprChecker->checkExpr(listener);
    return std::nullopt;
  }
  Signature signature;
  signature.params = {kind};
  signature.minArgs = 1;
  signature.ret = Type::voidTy();
  signature.voidResult = true;
  signature.asyncVoid = true;
  listener = this->closureChecker->checkFunctionExpr(function, &signature);

  return kind;
}

std::optional<const Type *>
EventTargetChecker::typedValueListener(CallExpr *call, ClassDecl *eventClass, const Type *object) {
  Expr *&listener = call->args[1];
  listener = this->exprChecker->checkExpr(listener);
  const Type *type = listener->type;
  if (!type->isFunction()) {
    listener = this->typeRelations->coerce(listener, object, "the listener");
    return std::make_optional<const Type *>(nullptr);
  }
  const Type *event = eventClass->type;
  bool noParams = type->params().empty();
  const Type *kind =
      noParams ? event : this->listenedEvent(type->params()[0], listener->loc(), eventClass);
  if (kind != nullptr) {
    const Type *heard = noParams ? event : kind;
    const Type *handler = Type::function({heard}, 1, false, Type::voidTy());
    listener = this->typeRelations->coerce(listener, handler);
  }

  return kind;
}

const Type *EventTargetChecker::listenedEvent(const Type *type, llvm::SMLoc loc,
                                              ClassDecl *eventClass) {
  if (type->isError()) {
    return nullptr;
  }
  ResultListenerEvent listened = EventTargetChecker::listenerEvent(type, eventClass);
  if (listened.isErr()) {
    this->state.getDiagnostics().error(loc, listened.error().message);
    return nullptr;
  }

  return type;
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
  const Type *ret = sym->getReturnType();
  FuncSymbol *callee = sym;
  if (sym->isGeneric() && !this->state.getFunctionStack().isInsideTrial() &&
      llvm::none_of(bound, [](const Type *t) { return TypeQueries::mentionsParam(t); })) {
    if (FuncSymbol *instance = this->genericInstantiator->instantiate(sym, bound, loc)) {
      callee = instance;
    }
  }
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
  std::optional<EventsFunction> function = EventsFunctionTable::find(fn);
  if (!function.has_value()) {
    return nullptr;
  }
  switch (*function) {
    case EventsFunction::GET_EVENT_LISTENERS:
      return this->checkListenersFunction(call, fn, loc, true);
    case EventsFunction::LISTENER_COUNT:
      return this->checkListenersFunction(call, fn, loc, false);
    case EventsFunction::ONCE:
    case EventsFunction::ON:
      return this->checkWaitFunction(call, fn, loc, *function == EventsFunction::ONCE);
  }

  return nullptr;
}

void EventTargetChecker::walkArgsFrom(CallExpr *call, size_t from) {
  for (size_t i = from; i < call->args.size(); ++i) {
    call->args[i] = this->exprChecker->checkExpr(call->args[i]);
  }
}

bool EventTargetChecker::isEventTarget(const Type *type) const {
  const ClassDecl *target = this->state.getLibraryDeclarations().findLibraryClass("EventTarget");
  const ClassDecl *cls = ClassDeclarer::classOf(type);

  return target != nullptr && cls != nullptr && cls->derivesFrom(target);
}

Expr *EventTargetChecker::checkListenersFunction(CallExpr *call, llvm::StringRef fn,
                                                 llvm::SMLoc loc, bool listeners) {
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
  if (listeners && this->isEventTarget(object->type)) {
    return this->targetListeners(call, object);
  }
  if (object->type->isError()) {
    this->walkArgsFrom(call, 1);
    return call;
  }
  ResultEmitterEvents emitter = this->emitterEvents(object->type, fn, listeners);
  if (emitter.isErr()) {
    this->state.getDiagnostics().error(object->loc(), emitter.error().message);
    this->walkArgsFrom(call, 1);
    return call;
  }
  llvm::StringRef methodName = listeners ? "listeners" : "listenerCount";
  auto *member = this->state.getAst().make<MemberExpr>(call->loc(), object, methodName);
  member->type = Type::error();
  auto *method =
      this->state.getAst().make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
  Expr *checked = this->emitterChecker->checkEmitterCall(
      method, member, ClassDeclarer::classOf(object->type), false);

  return checked != nullptr ? checked : method;
}

Expr *EventTargetChecker::targetListeners(CallExpr *call, Expr *object) {
  auto *member = this->state.getAst().make<MemberExpr>(call->loc(), object, "listenersOf");
  auto *method =
      this->state.getAst().make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
  method->type = Type::error();
  std::vector<bool> given{false};
  this->emitterChecker->callLibraryMethod(method, member, ClassDeclarer::classOf(object->type),
                                          "listenersOf", {}, &given, false);

  return method;
}

bool EventTargetChecker::hasTimeoutOption(const CallExpr *call) {
  if (call->args.size() != 3) {
    return false;
  }
  auto *options = dyn_cast<ObjectLitExpr>(call->args[2]);
  if (options == nullptr) {
    return false;
  }
  for (const ObjectLitExpr::Property &property : options->props) {
    if (!property.spread() && property.name == "timeout") {
      return true;
    }
  }

  return false;
}

Expr *EventTargetChecker::checkWaitFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                            bool once) {
  WaitCall wait{
      .call = call,
      .fn = fn,
      .loc = loc,
      .once = once,
      .timed = EventTargetChecker::hasTimeoutOption(call),
  };
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 2, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    this->walkArgsFrom(call, 1);
    return call;
  }
  wait.given = std::vector<bool>(call->args.size(), false);
  wait.given[0] = true;
  wait.given[1] = true;
  if (this->isEventTarget(type)) {
    return this->checkTargetWait(wait);
  }
  ResultEmitterEvents emitter = this->emitterEvents(type, fn, once);
  if (emitter.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), emitter.error().message);
    this->walkArgsFrom(call, 1);
    return call;
  }

  return this->checkEmitterWait(wait, emitter.value());
}

Expr *EventTargetChecker::checkTargetWait(WaitCall &wait) {
  CallExpr *call = wait.call;
  if (!wait.once) {
    std::string message = ErrorTextEventsFunction::targetOn();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    this->walkArgsFrom(call, 1);
    return call;
  }
  call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::string());

  return this->callLibraryFunction(call, "onceTarget", {}, wait.given, wait.loc);
}

Expr *EventTargetChecker::checkEmitterWait(WaitCall &wait, const Type *events) {
  CallExpr *call = wait.call;
  const Type *args = this->eventMaps->eventArgs(events, call->args[1], wait.fn);
  if (args == nullptr) {
    this->walkArgsFrom(call, 2);
    return call;
  }
  bool ends = this->waitEnds(call);
  if (!wait.timed && !ends) {
    auto *name = dyn_cast<NameExpr>(call->args[0]);
    llvm::StringRef shown = name != nullptr ? name->name : llvm::StringRef();
    std::string message = ErrorTextEventsFunction::endless(shown, wait.fn);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    this->walkArgsFrom(call, 2);
    return call;
  }
  if (wait.once && call->args.size() == 3 && !wait.timed) {
    std::string message = ErrorTextEventsFunction::options();
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    this->walkArgsFrom(call, 2);
    return call;
  }
  const Type *errors =
      events->isRecord() ? events->valueType() : this->eventMaps->findEventArgs(events, "error");
  if (wait.timed) {
    return this->checkTimedWait(wait, events, args, errors);
  }

  return this->checkPlainWait(wait, events, args, errors);
}

bool EventTargetChecker::waitEnds(const CallExpr *call) const {
  const ClassDecl *source = ClassDeclarer::classOf(call->args[0]->type);
  const ClassDecl *root =
      source != nullptr && source->generic() != nullptr ? source->generic() : source;
  bool ends = root != nullptr && root->library() && root->name != "EventEmitter" &&
              root->name != "EventEmitterBase";
  bool topLevel = this->state.getFunctionStack().getCurrentDecl() == nullptr &&
                  !this->state.getFunctionStack().isInsideAwaitLoop();
  bool runsOnce = topLevel || this->emitterChecker->ownsEmitter(call->args[0]);

  return ends || runsOnce;
}

Expr *EventTargetChecker::checkTimedWait(WaitCall &wait, const Type *events, const Type *args,
                                         const Type *errors) {
  CallExpr *call = wait.call;
  const Type *failure = Type::never();
  if (errors != nullptr) {
    this->state.getEventStore().markErrorListened(events);
    ResultOneErrorValue value = EventTargetChecker::oneErrorValue(errors, wait.fn);
    if (value.isErr()) {
      this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
      return call;
    }
    failure = value.value();
  }
  this->appendErrorsFlag(wait, errors != nullptr);
  std::string variant = wait.fn.str() + "Within";

  return this->callLibraryFunction(call, variant, {args, failure}, wait.given, wait.loc);
}

Expr *EventTargetChecker::checkPlainWait(WaitCall &wait, const Type *events, const Type *args,
                                         const Type *errors) {
  CallExpr *call = wait.call;
  size_t most = wait.once ? 2 : 3;
  while (call->args.size() < most) {
    call->args.push_back(this->state.getAst().make<UndefinedExpr>(call->loc()));
  }
  this->appendErrorsFlag(wait, errors != nullptr);
  if (errors == nullptr) {
    return this->callLibraryFunction(call, wait.fn, {args, Type::tuple({})}, wait.given, wait.loc);
  }
  this->state.getEventStore().markErrorListened(events);
  ResultOneErrorValue value = EventTargetChecker::oneErrorValue(errors, wait.fn);
  if (value.isErr()) {
    this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
    return call;
  }
  std::string variant = wait.fn.str() + "Result";

  return this->callLibraryFunction(call, variant, {args, value.value()}, wait.given, wait.loc);
}

void EventTargetChecker::appendErrorsFlag(WaitCall &wait, bool listensErrors) {
  CallExpr *call = wait.call;
  auto *flag = this->state.getAst().make<BoolLitExpr>(call->loc(), listensErrors);
  call->args.push_back(this->exprChecker->checkExpr(flag));
  wait.given.resize(call->args.size(), false);
  wait.given.back() = true;
}

Expr *EventTargetChecker::checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func) {
  if (!func->decl ||
      this->state.getLibraryDeclarations().findLibraryFunction(func->decl->name) != func->decl) {
    return nullptr;
  }

  return this->checkEventsFunction(call, func->decl->name, name->loc());
}

}
