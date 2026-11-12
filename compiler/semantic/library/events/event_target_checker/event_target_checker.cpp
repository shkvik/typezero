#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/program/library/builtin_modules/node_option_table/node_option_table.h"
#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "compiler/program/library/builtin_names/library_method_table/library_method_table.h"
#include "compiler/program/library/events/emitter_event_table/emitter_event_table.h"
#include "compiler/program/library/events/emitter_member_table/emitter_member_table.h"
#include "compiler/program/library/events/event_target_method_table/event_target_method_table.h"
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
  this->classDeclarer = &all.classDeclarer;
  this->closureChecker = &all.closureChecker;
  this->emitterChecker = &all.emitterChecker;
  this->argumentChecker = &all.argumentChecker;
  this->genericInstantiator = &all.genericInstantiator;
}

EventTargetChecker::ResultListenerEvent
EventTargetChecker::checkListenerEvent(const Type *type, const ClassDecl *eventClass) const {
  const auto *declared = this->classDeclarer->classOf(type);
  if (declared == nullptr || !declared->derivesFrom(eventClass) || type->isOptional()) {
    auto message = ErrorTextEventTarget::listener(type->name());

    return err(EventTargetCheckerListenerEventError{
        .code = EventTargetCheckerListenerEventErrorCode::NOT_AN_EVENT,
        .message = message,
    });
  }

  return ok(type);
}

EventTargetChecker::ResultOneErrorValue
EventTargetChecker::checkOneErrorValue(const Type *errors, llvm::StringRef functionName) const {
  if (!errors->isTuple() || errors->fields().size() != 1) {
    auto message = ErrorTextEventsFunction::errorValue(functionName, errors->name());

    return err(EventTargetCheckerOneErrorValueError{
        .code = EventTargetCheckerOneErrorValueErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }

  return ok(errors->fields().front().type);
}

EventTargetChecker::ResultEmitterEvents
EventTargetChecker::checkEmitterEvents(const Type *type, llvm::StringRef functionName,
                                       bool target) {
  const auto *events = this->eventMaps->eventMapOf(type);
  if (events == nullptr) {
    auto message = ErrorTextEventsFunction::notEmitter(functionName, target, type->name());

    return err(EventTargetCheckerEmitterEventsError{
        .code = EventTargetCheckerEmitterEventsErrorCode::NOT_AN_EMITTER,
        .message = message,
    });
  }

  return ok(events);
}

Expr *EventTargetChecker::checkEventTargetCall(CallExpr *call, MemberExpr *member,
                                               ClassDecl *classDecl) {
  auto method = EventTargetMethodTable::find(member->name);
  bool isAdd = method == EventTargetMethod::ADD_EVENT_LISTENER;
  if (!method.has_value()) {
    return nullptr;
  }
  auto *eventClass = this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::EVENT);
  auto *objectListener =
      this->state.getLibraryDeclarations().findLibraryType(LibraryType::EVENT_LISTENER_OBJECT);
  if (eventClass == nullptr || objectListener == nullptr) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 2, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  const auto *object = this->typeDeclarer->resolveAlias(objectListener);
  auto kind = this->typedListener(call, eventClass, object);
  if (!kind.has_value() || call->args[1]->type->isError()) {
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = true;
  given[1] = true;
  if (!isAdd) {
    call->args[1] = this->typeRelations->convert(call->args[1], Type::unknown());
    this->state.getInventoryStore().markComparesFunctions();
    auto removeName = LibraryMethodTable::getName(LibraryMethod::REMOVE_LISTENER_WITH);
    return this->emitterChecker->callLibraryMethod(call, member, classDecl, removeName, {}, &given,
                                                   CallReceiver::OWN);
  }
  if (*kind != nullptr) {
    auto addName = LibraryMethodTable::getName(LibraryMethod::ADD_FUNCTION_LISTENER);
    return this->emitterChecker->callLibraryMethod(call, member, classDecl, addName, {*kind},
                                                   &given, CallReceiver::OWN);
  }

  auto addName = LibraryMethodTable::getName(LibraryMethod::ADD_OBJECT_LISTENER);

  return this->emitterChecker->callLibraryMethod(call, member, classDecl, addName, {}, &given,
                                                 CallReceiver::OWN);
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
  bool isTypedParam = !params.empty() && params[0].type && !params[0].rest();
  if (isTypedParam) {
    const auto *written = this->typeResolver->resolveType(*params[0].type);
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
  bool isNoParams = type->params().empty();
  const Type *kind =
      isNoParams ? event : this->listenedEvent(type->params()[0], listener->loc(), eventClass);
  if (kind != nullptr) {
    const Type *heard = isNoParams ? event : kind;
    const auto *handler = Type::function({heard}, 1, false, Type::voidTy());
    listener = this->typeRelations->coerce(listener, handler);
  }

  return kind;
}

const Type *EventTargetChecker::listenedEvent(const Type *type, llvm::SMLoc loc,
                                              ClassDecl *eventClass) {
  if (type->isError()) {
    return nullptr;
  }
  auto listened = this->checkListenerEvent(type, eventClass);
  if (listened.isErr()) {
    this->state.getDiagnostics().error(loc, listened.error().message);
    return nullptr;
  }

  return type;
}

Expr *EventTargetChecker::callLibraryFunction(CallExpr *call, llvm::StringRef name,
                                              llvm::ArrayRef<const Type *> bound,
                                              const std::vector<bool> &given, llvm::SMLoc loc) {
  auto *functionDecl = this->state.getLibraryDeclarations().findLibraryFunction(name);
  assert(functionDecl != nullptr && functionDecl->sym != nullptr &&
         "a function of the emitter's code");

  return this->callLibrarySymbol(call, functionDecl->sym, bound, given, loc);
}

Expr *EventTargetChecker::callLibraryStatic(CallExpr *call, ClassDecl *classDecl,
                                            llvm::StringRef name,
                                            llvm::ArrayRef<const Type *> bound,
                                            const std::vector<bool> &given, llvm::SMLoc loc) {
  auto methods = this->classDeclarer->staticMethodsNamed(classDecl, name);
  assert(!methods.empty() && methods[0]->sym != nullptr && "a static method of the library");

  return this->callLibrarySymbol(call, methods[0]->sym, bound, given, loc);
}

Expr *EventTargetChecker::callLibrarySymbol(CallExpr *call, FuncSymbol *function,
                                            llvm::ArrayRef<const Type *> bound,
                                            const std::vector<bool> &given, llvm::SMLoc loc) {
  const auto *returnType = function->getReturnType();
  FuncSymbol *callee = function;
  if (function->isGeneric() && !this->state.getFunctionStack().isInsideTrial() &&
      llvm::none_of(bound, std::bind_front(&EventTargetChecker::isMentioningParam, this))) {
    auto *instance = this->genericInstantiator->instantiate(function, bound, loc);
    if (instance != nullptr) {
      callee = instance;
    }
  }
  call->target = FunctionCall{.func = callee};
  call->type = returnType != nullptr
                   ? this->typeParams->substitute(returnType, function->typeParams, bound)
                   : Type::error();
  std::vector<const Type *> params = callee->params;
  if (callee == function) {
    for (const Type *&p : params) {
      p = this->typeParams->substitute(p, function->typeParams, bound);
    }
  }

  return this->argumentChecker->checkArguments(call, params, callee->minArgs, callee->hasRest, loc,
                                               &given);
}

Expr *EventTargetChecker::checkEventsFunction(CallExpr *call, llvm::StringRef functionName,
                                              llvm::SMLoc loc) {
  if (this->libraryCalls->isInLibrary(call->loc())) {
    return nullptr;
  }
  call->type = Type::error();
  auto function = EventsFunctionTable::find(functionName);
  if (!function.has_value()) {
    return nullptr;
  }
  switch (*function) {
    case EventsFunction::GET_EVENT_LISTENERS:
      return this->checkListenersFunction(call, functionName, loc, ListenersQuery::LISTENERS);
    case EventsFunction::LISTENER_COUNT:
      return this->checkListenersFunction(call, functionName, loc, ListenersQuery::COUNT);
    case EventsFunction::ONCE:
    case EventsFunction::ON:
      return this->checkWaitFunction(call, functionName, loc, *function == EventsFunction::ONCE);
  }

  return nullptr;
}

void EventTargetChecker::walkArgsFrom(CallExpr *call, size_t from) {
  for (size_t index = from; index < call->args.size(); ++index) {
    call->args[index] = this->exprChecker->checkExpr(call->args[index]);
  }
}

bool EventTargetChecker::isEventTarget(const Type *type) const {
  const auto *target =
      this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::EVENT_TARGET);
  const auto *classDecl = this->classDeclarer->classOf(type);

  return target != nullptr && classDecl != nullptr && classDecl->derivesFrom(target);
}

Expr *EventTargetChecker::checkListenersFunction(CallExpr *call, llvm::StringRef functionName,
                                                 llvm::SMLoc loc, ListenersQuery query) {
  bool isListeners = query == ListenersQuery::LISTENERS;
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  auto *object = this->exprChecker->checkExpr(call->args[0]);
  if (isListeners && this->isEventTarget(object->type)) {
    return this->targetListeners(call, object);
  }
  if (object->type->isError()) {
    this->walkArgsFrom(call, 1);
    return call;
  }
  auto emitter = this->checkEmitterEvents(object->type, functionName, isListeners);
  if (emitter.isErr()) {
    this->state.getDiagnostics().error(object->loc(), emitter.error().message);
    this->walkArgsFrom(call, 1);
    return call;
  }
  auto emitterMember = isListeners ? EmitterMember::LISTENERS : EmitterMember::LISTENER_COUNT;
  auto methodName = EmitterMemberTable::getName(emitterMember);
  auto *member = this->state.getAst().make<MemberExpr>(call->loc(), object, methodName);
  member->type = Type::error();
  auto *method =
      this->state.getAst().make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
  auto *emitterClass = this->classDeclarer->classOf(object->type);
  auto *checked =
      this->emitterChecker->checkEmitterCall(method, member, emitterClass, CallReceiver::OWN);

  return checked != nullptr ? checked : method;
}

Expr *EventTargetChecker::targetListeners(CallExpr *call, Expr *object) {
  auto listenersOf = LibraryMethodTable::getName(LibraryMethod::LISTENERS_OF);
  auto *member = this->state.getAst().make<MemberExpr>(call->loc(), object, listenersOf);
  auto *method =
      this->state.getAst().make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
  method->type = Type::error();
  std::vector<bool> given{false};
  auto *targetClass = this->classDeclarer->classOf(object->type);
  this->emitterChecker->callLibraryMethod(method, member, targetClass, listenersOf, {}, &given,
                                          CallReceiver::OWN);

  return method;
}

bool EventTargetChecker::hasTimeoutOption(const CallExpr *call) const {
  if (call->args.size() != 3) {
    return false;
  }
  auto *options = dyn_cast<ObjectLitExpr>(call->args[2]);
  if (options == nullptr) {
    return false;
  }
  for (const ObjectLitExpr::Property &property : options->props) {
    if (!property.spread() && NodeOptionTable::find(property.name) == NodeOption::TIMEOUT) {
      return true;
    }
  }

  return false;
}

Expr *EventTargetChecker::checkWaitFunction(CallExpr *call, llvm::StringRef functionName,
                                            llvm::SMLoc loc, bool once) {
  WaitCall wait{
      .call = call,
      .fn = functionName,
      .loc = loc,
      .once = once,
      .timed = this->hasTimeoutOption(call),
  };
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 2, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (isRejected || arity.isErr()) {
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
  auto emitter = this->checkEmitterEvents(type, functionName, once);
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
    auto message = ErrorTextEventsFunction::targetOn();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    this->walkArgsFrom(call, 1);
    return call;
  }
  call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::string());

  auto onceTarget = LibraryFunctionTable::getName(LibraryFunction::ONCE_TARGET);

  return this->callLibraryFunction(call, onceTarget, {}, wait.given, wait.loc);
}

Expr *EventTargetChecker::checkEmitterWait(WaitCall &wait, const Type *events) {
  CallExpr *call = wait.call;
  const auto *args = this->eventMaps->eventArgs(events, call->args[1], wait.fn);
  if (args == nullptr) {
    this->walkArgsFrom(call, 2);
    return call;
  }
  bool isEnding = this->isWaitEnding(call);
  if (!wait.timed && !isEnding) {
    auto *name = dyn_cast<NameExpr>(call->args[0]);
    llvm::StringRef shown = name != nullptr ? name->name : llvm::StringRef();
    auto message = ErrorTextEventsFunction::endless(shown, wait.fn);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    this->walkArgsFrom(call, 2);
    return call;
  }
  if (wait.once && call->args.size() == 3 && !wait.timed) {
    auto message = ErrorTextEventsFunction::options();
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    this->walkArgsFrom(call, 2);
    return call;
  }
  auto errorName = EmitterEventTable::getName(EmitterEvent::ERROR_EVENT);
  const Type *errors =
      events->isRecord() ? events->valueType() : this->eventMaps->findEventArgs(events, errorName);
  if (wait.timed) {
    return this->checkTimedWait(wait, events, args, errors);
  }

  return this->checkPlainWait(wait, events, args, errors);
}

bool EventTargetChecker::isWaitEnding(const CallExpr *call) const {
  const auto *source = this->classDeclarer->classOf(call->args[0]->type);
  const ClassDecl *root =
      source != nullptr && source->generic() != nullptr ? source->generic() : source;
  bool isEmitterRoot =
      root != nullptr && (LibraryClassTable::isName(root->name, LibraryClass::EVENT_EMITTER) ||
                          LibraryClassTable::isName(root->name, LibraryClass::EVENT_EMITTER_BASE));
  bool isEnding = root != nullptr && root->library() && !isEmitterRoot;
  bool isTopLevel = this->state.getFunctionStack().getCurrentDecl() == nullptr &&
                    !this->state.getFunctionStack().isInsideAwaitLoop();
  bool isRunningOnce = isTopLevel || this->emitterChecker->isOwningEmitter(call->args[0]);

  return isEnding || isRunningOnce;
}

Expr *EventTargetChecker::checkTimedWait(WaitCall &wait, const Type *events, const Type *args,
                                         const Type *errors) {
  CallExpr *call = wait.call;
  const auto *failure = Type::never();
  if (errors != nullptr) {
    this->state.getEventStore().markErrorListened(events);
    auto value = this->checkOneErrorValue(errors, wait.fn);
    if (value.isErr()) {
      this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
      return call;
    }
    failure = value.value();
  }
  this->appendErrorsFlag(wait, errors != nullptr);
  auto within = wait.once ? LibraryFunction::ONCE_WITHIN : LibraryFunction::ON_WITHIN;
  auto variant = LibraryFunctionTable::getName(within);

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
  auto value = this->checkOneErrorValue(errors, wait.fn);
  if (value.isErr()) {
    this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
    return call;
  }
  auto result = wait.once ? LibraryFunction::ONCE_RESULT : LibraryFunction::ON_RESULT;
  auto variant = LibraryFunctionTable::getName(result);

  return this->callLibraryFunction(call, variant, {args, value.value()}, wait.given, wait.loc);
}

void EventTargetChecker::appendErrorsFlag(WaitCall &wait, bool listensErrors) {
  CallExpr *call = wait.call;
  auto *flag = this->state.getAst().make<BoolLitExpr>(call->loc(), listensErrors);
  call->args.push_back(this->exprChecker->checkExpr(flag));
  wait.given.resize(call->args.size(), false);
  wait.given.back() = true;
}

Expr *EventTargetChecker::checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *function) {
  if (function->decl == nullptr || this->state.getLibraryDeclarations().findLibraryFunction(
                                       function->decl->name) != function->decl) {
    return nullptr;
  }

  return this->checkEventsFunction(call, function->decl->name, name->loc());
}

bool EventTargetChecker::isMentioningParam(const Type *type) const {
  return TypeQueries::mentionsParam(type);
}

}
