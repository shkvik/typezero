#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/library/events/event_map_inference/event_map_inference.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;

namespace tz {

EventMaps::EventMaps(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->argumentChecker = &all.argumentChecker;
  this->eventMapInference = &all.eventMapInference;
  this->keyedFieldChecker = &all.keyedFieldChecker;
  this->functionValueChecker = &all.functionValueChecker;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool EventMaps::isServiceEvent(llvm::StringRef name) {
  return name == "newListener" || name == "removeListener";
}

const Type *EventMaps::serviceEventArgs() {
  return Type::tuple(
      {Type::object({{"event", Type::string(), false}, {"listener", Type::unknown(), false}})});
}

bool EventMaps::carriesOne(const Type *args) {
  return args->fields().size() <= 1 && !args->tupleRest();
}

EventMaps::ResultListenerThis EventMaps::listenerThis(const Type *self, const Type *receiver) {
  if (!self->isUnknown() && !receiver->isSubtypeOf(self)) {
    std::string message = ErrorTextListener::thisType(self->name(), receiver->name());

    return err(EventMapsListenerThisError{
        .code = EventMapsListenerThisErrorCode::OTHER_RECEIVER,
        .message = message,
    });
  }

  return ok(self);
}

std::string EventMaps::nearestEvent(const Type *events, llvm::StringRef name) {
  std::string best;
  unsigned bestDistance = ~0u;
  for (const Field &f : events->fields()) {
    unsigned distance = llvm::StringRef(f.name).edit_distance(name);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = f.name;
    }
  }

  return bestDistance <= std::max<size_t>(2, name.size() / 3) ? best : std::string();
}

bool EventMaps::isEmitterClass(const ClassDecl *cls) const {
  ClassDecl *base = this->state.getLibraryDeclarations().findLibraryClass("EventEmitterBase");

  return cls && base && cls->derivesFrom(base);
}

const Type *EventMaps::eventMapOf(const Type *type) const {
  const ClassDecl *emitter = this->state.getLibraryDeclarations().findLibraryClass("EventEmitter");
  if (!emitter) {
    return nullptr;
  }
  const ClassDecl *streams = this->state.getLibraryDeclarations().findLibraryClass("StreamEmitter");
  for (const ClassDecl *c = ClassDeclarer::classOf(type); c; c = c->parent) {
    if (c->generic() && (c->generic() == emitter || c->generic() == streams) &&
        !c->typeArgs.empty()) {
      return c->typeArgs[0];
    }
  }

  return nullptr;
}

EventMaps::ResultEventMapFits EventMaps::eventMapFits(const Type *events) {
  if (events->isError() || TypeQueries::mentionsParam(events)) {
    return ok(events);
  }
  if (events->isRecord()) {
    return EventMaps::recordMapFits(events);
  }
  bool isNotObject = !events->isObject() || events->isClass() || events->isTuple() ||
                     events->isAnyObject() || events->isBytes() || events->isDate() ||
                     events->isRegExp() || events->isMatch();
  if (isNotObject) {
    std::string why = ErrorTextEventMap::notOne(events->name());
    std::string message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  for (const Field &f : events->fields()) {
    ResultEventMapFits fits = EventMaps::eventFieldFits(f);
    if (fits.isErr()) {
      return fits;
    }
  }

  return ok(events);
}

EventMaps::ResultEventMapFits EventMaps::recordMapFits(const Type *events) {
  const Type *args = events->valueType();
  if (!args->isTuple()) {
    std::string why = ErrorTextEventMap::notTuple(args->name());
    std::string message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!EventMaps::carriesOne(args)) {
    std::string event = ErrorTextEventMap::eachEvent();
    std::string message = ErrorTextEventMap::several(event, args->name());

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }

  return ok(events);
}

EventMaps::ResultEventMapFits EventMaps::eventFieldFits(const Field &f) {
  bool isService = EventMaps::isServiceEvent(f.name);
  if (f.type->isOptional()) {
    std::string why = ErrorTextEventMap::optional(f.name);
    std::string message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!f.type->isTuple()) {
    std::string why = ErrorTextEventMap::fieldTuple(f.name, f.type->name());
    std::string message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!isService && !EventMaps::carriesOne(f.type)) {
    std::string event = ErrorTextEventMap::event(f.name);
    std::string message = ErrorTextEventMap::several(event, f.type->name());

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }
  if (isService && f.type != EventMaps::serviceEventArgs()) {
    std::string message = ErrorTextEventMap::service(f.name, f.type->name());

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::SERVICE_EVENT,
        .message = message,
    });
  }

  return ok(f.type);
}

std::optional<std::string> EventMaps::eventKey(Expr *&name, llvm::StringRef method) {
  name = this->argumentChecker->checkArg(name, Type::string());
  std::optional<std::string> key = this->keyedFieldChecker->constantKey(name);
  if (!key && !name->type->isError()) {
    std::string message = ErrorTextEventName::unknown(method);
    this->state.getDiagnostics().error(name->loc(), message);
  }

  return key;
}

const Type *EventMaps::eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                                 std::string *label) {
  if (events->isRecord()) {
    if (auto *lit = dyn_cast<StringLitExpr>(name); lit && label) {
      *label = ToUtf8::toUtf8(lit->value);
    }
    name = this->argumentChecker->checkArg(name, Type::string());
    return name->type->isError() ? nullptr : events->valueType();
  }
  std::optional<std::string> key = this->eventKey(name, method);
  if (!key) {
    return nullptr;
  }
  if (label) {
    *label = *key;
  }
  llvm::SMLoc loc = name->loc();
  if (EventMaps::isServiceEvent(*key)) {
    return EventMaps::serviceEventArgs();
  }
  EventSite *site = this->state.getEventStore().findSiteOfMap(events);
  if (site != nullptr) {
    return this->inferredEventArgs(site, *key, loc);
  }
  if (const Field *f = events->field(*key)) {
    return f->type;
  }
  std::string nearest = EventMaps::nearestEvent(events, *key);
  std::string message = ErrorTextEventName::noEvent(*key, events, nearest);
  this->state.getDiagnostics().error(loc, message);

  return nullptr;
}

const Type *EventMaps::inferredEventArgs(EventSite *site, const std::string &key, llvm::SMLoc loc) {
  EventRecord *record = this->eventMapInference->completeEvent(site, key);
  if (record->args != nullptr) {
    return record->args;
  }
  if (record->state == EventState::COMPLETING) {
    std::string message = ErrorTextEventName::circular(key);
    this->state.getDiagnostics().error(loc, message);
    return nullptr;
  }
  if (!record->untyped.empty()) {
    std::string message = ErrorTextEventName::untyped(key, record->untyped);
    this->state.getDiagnostics().error(loc, message);
    return nullptr;
  }
  this->eventMapInference->completeSite(site);
  std::string nearest = EventMaps::nearestEvent(site->map, key);
  std::string message = ErrorTextEventName::notEmitted(key, site->map, nearest);
  this->state.getDiagnostics().error(loc, message);

  return nullptr;
}

const Type *EventMaps::findEventArgs(const Type *events, llvm::StringRef name) {
  EventSite *site = this->state.getEventStore().findSiteOfMap(events);
  if (site != nullptr) {
    EventRecord *record = this->eventMapInference->completeEvent(site, name);
    return record->args;
  }
  const Field *f = events->field(name);

  return f != nullptr ? f->type : nullptr;
}

const Type *EventMaps::listenerType(const Type *args, const Type *ret) {
  std::vector<const Type *> params;
  for (const Field &f : args->fields()) {
    params.push_back(f.type);
  }
  bool hasRest = args->tupleRest() != nullptr;
  if (hasRest) {
    params.push_back(args->tupleRest());
  }

  return Type::function(params, args->tupleMin(), hasRest, ret);
}

Expr *EventMaps::checkListener(Expr *arg, const Type *args, llvm::StringRef event,
                               const Type *receiver, ListenerKind &kind) {
  ListenerWant want{
      .args = args,
      .event = event,
      .receiver = receiver,
      .sync = EventMaps::listenerType(args, Type::voidTy()),
      .async = EventMaps::listenerType(args, Type::promise(Type::voidTy())),
  };
  kind = ListenerKind::Plain;
  auto *fe = dyn_cast<FunctionExpr>(arg);
  if (fe != nullptr) {
    return this->checkListenerLiteral(fe, want, kind);
  }
  Expr *checked = this->exprChecker->checkExpr(arg);

  return this->checkListenerValue(checked, want, kind);
}

bool EventMaps::listenerTakesThis(const ListenerWant &want, const Type *self, llvm::SMLoc loc) {
  if (self->isError()) {
    return false;
  }
  ResultListenerThis taken = EventMaps::listenerThis(self, want.receiver);
  if (taken.isErr()) {
    this->state.getDiagnostics().error(loc, taken.error().message);
    return false;
  }

  return true;
}

const Type *EventMaps::withThis(const ListenerWant &want, const Type *self) {
  const Type *sync = want.sync;
  std::vector<const Type *> params{self};
  params.insert(params.end(), sync->params().begin(), sync->params().end());

  return Type::function(params, sync->minArgs() + 1, sync->hasRest(), Type::voidTy(), true);
}

Expr *EventMaps::checkListenerLiteral(FunctionExpr *fe, const ListenerWant &want,
                                      ListenerKind &kind) {
  FunctionDecl *fn = fe->fn;
  bool isExplicitThis =
      !fn->params.empty() && fn->params[0].name == "this" && fn->params[0].type.has_value();
  if (isExplicitThis) {
    return this->checkThisListener(fe, want, kind);
  }
  if (fn->isAsync) {
    kind = ListenerKind::Async;
  }
  const Type *wanted = fn->isAsync ? want.async : want.sync;
  Signature sig = ClosureChecker::signatureOf(wanted);
  sig.voidResult = !fn->isAsync;
  Expr *checked = this->closureChecker->checkFunctionExpr(fe, &sig);
  if (checked->type->isError()) {
    return checked;
  }
  Expr *coerced = this->typeRelations->coerce(checked, wanted);

  return this->typeRelations->convert(coerced, Type::unknown());
}

Expr *EventMaps::checkThisListener(FunctionExpr *fe, const ListenerWant &want, ListenerKind &kind) {
  FunctionDecl *fn = fe->fn;
  const Type *self = this->typeResolver->resolveType(*fn->params[0].type, false);
  if (!this->listenerTakesThis(want, self, fn->params[0].loc)) {
    return this->exprChecker->checkExpr(fe);
  }
  if (fn->isAsync) {
    std::string message = ErrorTextListener::asyncThis();
    this->state.getDiagnostics().error(fe->loc(), message);
    return this->exprChecker->checkExpr(fe);
  }
  kind = ListenerKind::This;
  const Type *wanted = EventMaps::withThis(want, self);
  Signature sig = ClosureChecker::signatureOf(wanted);
  sig.voidResult = true;
  Expr *checked = this->closureChecker->checkFunctionExpr(fe, &sig);
  if (checked->type->isError()) {
    return checked;
  }
  Expr *coerced = this->typeRelations->coerce(checked, wanted);

  return this->typeRelations->convert(coerced, Type::unknown());
}

Expr *EventMaps::checkListenerValue(Expr *arg, const ListenerWant &want, ListenerKind &kind) {
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isFunction()) {
    std::string message = ErrorTextListener::notFunc(type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  if (type->hasThis()) {
    return this->checkThisValue(arg, want, kind);
  }
  bool isAsync = type->ret()->isPromise();
  if (isAsync) {
    kind = ListenerKind::Async;
  }
  const Type *wanted = isAsync ? want.async : want.sync;
  if (type == wanted) {
    return this->typeRelations->convert(arg, Type::unknown());
  }
  if (isAsync && type->ret() != want.async->ret()) {
    std::string message = ErrorTextListener::asyncRet(type->ret()->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  Expr *adapted = this->functionValueChecker->adaptFunction(arg, wanted);
  if (adapted != nullptr) {
    return this->typeRelations->convert(adapted, Type::unknown());
  }
  std::string message = ErrorTextListener::getsArrow(want.event, want.args->name(), type->name());
  this->state.getDiagnostics().error(arg->loc(), message);

  return arg;
}

Expr *EventMaps::checkThisValue(Expr *arg, const ListenerWant &want, ListenerKind &kind) {
  const Type *type = arg->type;
  const Type *self = type->params()[0];
  if (!this->listenerTakesThis(want, self, arg->loc())) {
    return arg;
  }
  kind = ListenerKind::This;
  if (type == EventMaps::withThis(want, self)) {
    return this->typeRelations->convert(arg, Type::unknown());
  }
  std::string message = ErrorTextListener::gets(want.event, want.args->name(), type->name());
  this->state.getDiagnostics().error(arg->loc(), message);

  return arg;
}

const Type *EventMaps::createdType(const NewExpr *create) {
  if (!create->member.empty()) {
    return nullptr;
  }
  auto *cls = llvm::dyn_cast_or_null<ClassDecl>(this->typeDeclarer->lookupType(create->name));
  if (!cls) {
    return nullptr;
  }
  if (!cls->isGeneric()) {
    return cls->type;
  }
  std::vector<const Type *> args;
  for (const TypeRef &ref : create->typeArgs) {
    args.push_back(this->typeResolver->resolveType(ref, false));
    if (args.back()->isError()) {
      return nullptr;
    }
  }
  if (args.empty() && cls->library() && cls->name == "EventEmitter") {
    Type *events = this->eventMapInference->newEventMap(create->loc());
    args.push_back(events);
  }
  if (args.empty()) {
    return nullptr;
  }
  GenericClassInstantiator::ResultClassMade made =
      this->genericClassInstantiator->instantiateClass(cls, std::move(args), create->loc());
  if (made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? cls->loc()
            : create->loc();
    this->state.getDiagnostics().error(at, made.error().message);
  }

  return made.isOk() ? made.value()->type : nullptr;
}

}
