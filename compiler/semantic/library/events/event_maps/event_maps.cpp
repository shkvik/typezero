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
  this->classDeclarer = &all.classDeclarer;
  this->closureChecker = &all.closureChecker;
  this->argumentChecker = &all.argumentChecker;
  this->eventMapInference = &all.eventMapInference;
  this->keyedFieldChecker = &all.keyedFieldChecker;
  this->functionValueChecker = &all.functionValueChecker;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool EventMaps::isServiceEvent(llvm::StringRef name) const {
  return name == "newListener" || name == "removeListener";
}

const Type *EventMaps::serviceEventArgs() const {
  return Type::tuple(
      {Type::object({{"event", Type::string(), false}, {"listener", Type::unknown(), false}})});
}

bool EventMaps::isCarryingOne(const Type *args) const {
  return args->fields().size() <= 1 && args->tupleRest() == nullptr;
}

EventMaps::ResultListenerThis EventMaps::checkListenerThis(const Type *self,
                                                           const Type *receiver) const {
  if (!self->isUnknown() && !receiver->isSubtypeOf(self)) {
    auto message = ErrorTextListener::thisType(self->name(), receiver->name());

    return err(EventMapsListenerThisError{
        .code = EventMapsListenerThisErrorCode::OTHER_RECEIVER,
        .message = message,
    });
  }

  return ok(self);
}

std::string EventMaps::nearestEvent(const Type *events, llvm::StringRef name) const {
  auto best = std::string();
  unsigned bestDistance = ~0u;
  for (const Field &field : events->fields()) {
    unsigned distance = llvm::StringRef(field.name).edit_distance(name);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = field.name;
    }
  }

  return bestDistance <= std::max<size_t>(2, name.size() / 3) ? best : std::string();
}

bool EventMaps::isEmitterClass(const ClassDecl *classDecl) const {
  auto *base =
      this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::EVENT_EMITTER_BASE);

  return classDecl != nullptr && base != nullptr && classDecl->derivesFrom(base);
}

const Type *EventMaps::eventMapOf(const Type *type) const {
  const auto *emitter =
      this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::EVENT_EMITTER);
  if (emitter == nullptr) {
    return nullptr;
  }
  const auto *streams =
      this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::STREAM_EMITTER);
  for (const ClassDecl *classDecl = this->classDeclarer->classOf(type); classDecl != nullptr;
       classDecl = classDecl->parent) {
    if (classDecl->generic() != nullptr &&
        (classDecl->generic() == emitter || classDecl->generic() == streams) &&
        !classDecl->typeArgs.empty()) {
      return classDecl->typeArgs[0];
    }
  }

  return nullptr;
}

EventMaps::ResultEventMapFits EventMaps::checkEventMapFits(const Type *events) const {
  if (events->isError() || TypeQueries::mentionsParam(events)) {
    return ok(events);
  }
  if (events->isRecord()) {
    return this->checkRecordMapFits(events);
  }
  bool isNotObject = !events->isObject() || events->isClass() || events->isTuple() ||
                     events->isAnyObject() || events->isBytes() || events->isDate() ||
                     events->isRegExp() || events->isMatch();
  if (isNotObject) {
    auto why = ErrorTextEventMap::notOne(events->name());
    auto message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  for (const Field &field : events->fields()) {
    auto fits = this->checkEventFieldFits(field);
    if (fits.isErr()) {
      return fits;
    }
  }

  return ok(events);
}

EventMaps::ResultEventMapFits EventMaps::checkRecordMapFits(const Type *events) const {
  const Type *args = events->valueType();
  if (!args->isTuple()) {
    auto why = ErrorTextEventMap::notTuple(args->name());
    auto message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!this->isCarryingOne(args)) {
    auto event = ErrorTextEventMap::eachEvent();
    auto message = ErrorTextEventMap::several(event, args->name());

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }

  return ok(events);
}

EventMaps::ResultEventMapFits EventMaps::checkEventFieldFits(const Field &field) const {
  bool isService = this->isServiceEvent(field.name);
  if (field.type->isOptional()) {
    auto why = ErrorTextEventMap::optional(field.name);
    auto message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!field.type->isTuple()) {
    auto why = ErrorTextEventMap::fieldTuple(field.name, field.type->name());
    auto message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!isService && !this->isCarryingOne(field.type)) {
    auto event = ErrorTextEventMap::event(field.name);
    auto message = ErrorTextEventMap::several(event, field.type->name());

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }
  if (isService && field.type != this->serviceEventArgs()) {
    auto message = ErrorTextEventMap::service(field.name, field.type->name());

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::SERVICE_EVENT,
        .message = message,
    });
  }

  return ok(field.type);
}

std::optional<std::string> EventMaps::eventKey(Expr *&name, llvm::StringRef method) {
  name = this->argumentChecker->checkArg(name, Type::string());
  auto key = this->keyedFieldChecker->constantKey(name);
  if (!key && !name->type->isError()) {
    auto message = ErrorTextEventName::unknown(method);
    this->state.getDiagnostics().error(name->loc(), message);
  }

  return key;
}

const Type *EventMaps::eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                                 std::string *label) {
  if (events->isRecord()) {
    auto *literal = dyn_cast<StringLitExpr>(name);
    if (literal != nullptr && label) {
      *label = ToUtf8::toUtf8(literal->value);
    }
    name = this->argumentChecker->checkArg(name, Type::string());
    return name->type->isError() ? nullptr : events->valueType();
  }
  auto key = this->eventKey(name, method);
  if (!key) {
    return nullptr;
  }
  if (label) {
    *label = *key;
  }
  llvm::SMLoc loc = name->loc();
  if (this->isServiceEvent(*key)) {
    return this->serviceEventArgs();
  }
  auto *site = this->state.getEventStore().findSiteOfMap(events);
  if (site != nullptr) {
    return this->inferredEventArgs(site, *key, loc);
  }
  const auto *field = events->field(*key);
  if (field != nullptr) {
    return field->type;
  }
  auto nearest = this->nearestEvent(events, *key);
  auto message = ErrorTextEventName::noEvent(*key, events, nearest);
  this->state.getDiagnostics().error(loc, message);

  return nullptr;
}

const Type *EventMaps::inferredEventArgs(EventSite *site, const std::string &key, llvm::SMLoc loc) {
  auto *record = this->eventMapInference->completeEvent(site, key);
  if (record->args != nullptr) {
    return record->args;
  }
  if (record->state == EventState::COMPLETING) {
    auto message = ErrorTextEventName::circular(key);
    this->state.getDiagnostics().error(loc, message);
    return nullptr;
  }
  if (!record->untyped.empty()) {
    auto message = ErrorTextEventName::untyped(key, record->untyped);
    this->state.getDiagnostics().error(loc, message);
    return nullptr;
  }
  this->eventMapInference->completeSite(site);
  auto nearest = this->nearestEvent(site->map, key);
  auto message = ErrorTextEventName::notEmitted(key, site->map, nearest);
  this->state.getDiagnostics().error(loc, message);

  return nullptr;
}

const Type *EventMaps::findEventArgs(const Type *events, llvm::StringRef name) {
  auto *site = this->state.getEventStore().findSiteOfMap(events);
  if (site != nullptr) {
    auto *record = this->eventMapInference->completeEvent(site, name);
    return record->args;
  }
  const auto *field = events->field(name);

  return field != nullptr ? field->type : nullptr;
}

const Type *EventMaps::listenerType(const Type *args, const Type *returnType) const {
  auto params = std::vector<const Type *>();
  for (const Field &field : args->fields()) {
    params.push_back(field.type);
  }
  bool hasRest = args->tupleRest() != nullptr;
  if (hasRest) {
    params.push_back(args->tupleRest());
  }

  return Type::function(params, args->tupleMin(), hasRest, returnType);
}

Expr *EventMaps::checkListener(Expr *arg, const Type *args, llvm::StringRef event,
                               const Type *receiver, ListenerKind &kind) {
  ListenerWant want{
      .args = args,
      .event = event,
      .receiver = receiver,
      .sync = this->listenerType(args, Type::voidTy()),
      .async = this->listenerType(args, Type::promise(Type::voidTy())),
  };
  kind = ListenerKind::Plain;
  auto *functionExpr = dyn_cast<FunctionExpr>(arg);
  if (functionExpr != nullptr) {
    return this->checkListenerLiteral(functionExpr, want, kind);
  }
  auto *checked = this->exprChecker->checkExpr(arg);

  return this->checkListenerValue(checked, want, kind);
}

bool EventMaps::isListenerTakingThis(const ListenerWant &want, const Type *self, llvm::SMLoc loc) {
  if (self->isError()) {
    return false;
  }
  auto taken = this->checkListenerThis(self, want.receiver);
  if (taken.isErr()) {
    this->state.getDiagnostics().error(loc, taken.error().message);
    return false;
  }

  return true;
}

const Type *EventMaps::withThis(const ListenerWant &want, const Type *self) const {
  const Type *sync = want.sync;
  std::vector<const Type *> params{self};
  params.insert(params.end(), sync->params().begin(), sync->params().end());

  return Type::function(params, sync->minArgs() + 1, sync->hasRest(), Type::voidTy(), true);
}

Expr *EventMaps::checkListenerLiteral(FunctionExpr *functionExpr, const ListenerWant &want,
                                      ListenerKind &kind) {
  FunctionDecl *functionDecl = functionExpr->fn;
  bool isExplicitThis = !functionDecl->params.empty() && functionDecl->params[0].name == "this" &&
                        functionDecl->params[0].type.has_value();
  if (isExplicitThis) {
    return this->checkThisListener(functionExpr, want, kind);
  }
  if (functionDecl->isAsync) {
    kind = ListenerKind::Async;
  }
  const Type *wanted = functionDecl->isAsync ? want.async : want.sync;
  auto signature = this->closureChecker->signatureOf(wanted);
  signature.voidResult = !functionDecl->isAsync;
  auto *checked = this->closureChecker->checkFunctionExpr(functionExpr, &signature);
  if (checked->type->isError()) {
    return checked;
  }
  auto *coerced = this->typeRelations->coerce(checked, wanted);

  return this->typeRelations->convert(coerced, Type::unknown());
}

Expr *EventMaps::checkThisListener(FunctionExpr *functionExpr, const ListenerWant &want,
                                   ListenerKind &kind) {
  FunctionDecl *functionDecl = functionExpr->fn;
  const auto *self = this->typeResolver->resolveType(*functionDecl->params[0].type);
  if (!this->isListenerTakingThis(want, self, functionDecl->params[0].loc)) {
    return this->exprChecker->checkExpr(functionExpr);
  }
  if (functionDecl->isAsync) {
    auto message = ErrorTextListener::asyncThis();
    this->state.getDiagnostics().error(functionExpr->loc(), message);
    return this->exprChecker->checkExpr(functionExpr);
  }
  kind = ListenerKind::This;
  const auto *wanted = this->withThis(want, self);
  auto signature = this->closureChecker->signatureOf(wanted);
  signature.voidResult = true;
  auto *checked = this->closureChecker->checkFunctionExpr(functionExpr, &signature);
  if (checked->type->isError()) {
    return checked;
  }
  auto *coerced = this->typeRelations->coerce(checked, wanted);

  return this->typeRelations->convert(coerced, Type::unknown());
}

Expr *EventMaps::checkListenerValue(Expr *arg, const ListenerWant &want, ListenerKind &kind) {
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isFunction()) {
    auto message = ErrorTextListener::notFunc(type->name());
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
    auto message = ErrorTextListener::asyncRet(type->ret()->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  auto *adapted = this->functionValueChecker->adaptFunction(arg, wanted);
  if (adapted != nullptr) {
    return this->typeRelations->convert(adapted, Type::unknown());
  }
  auto message = ErrorTextListener::getsArrow(want.event, want.args->name(), type->name());
  this->state.getDiagnostics().error(arg->loc(), message);

  return arg;
}

Expr *EventMaps::checkThisValue(Expr *arg, const ListenerWant &want, ListenerKind &kind) {
  const Type *type = arg->type;
  const Type *self = type->params()[0];
  if (!this->isListenerTakingThis(want, self, arg->loc())) {
    return arg;
  }
  kind = ListenerKind::This;
  if (type == this->withThis(want, self)) {
    return this->typeRelations->convert(arg, Type::unknown());
  }
  auto message = ErrorTextListener::gets(want.event, want.args->name(), type->name());
  this->state.getDiagnostics().error(arg->loc(), message);

  return arg;
}

const Type *EventMaps::createdType(const NewExpr *create) {
  if (!create->member.empty()) {
    return nullptr;
  }
  auto *classDecl = llvm::dyn_cast_or_null<ClassDecl>(this->typeDeclarer->lookupType(create->name));
  if (classDecl == nullptr) {
    return nullptr;
  }
  if (!classDecl->isGeneric()) {
    return classDecl->type;
  }
  auto args = std::vector<const Type *>();
  for (const TypeRef &ref : create->typeArgs) {
    args.push_back(this->typeResolver->resolveType(ref));
    if (args.back()->isError()) {
      return nullptr;
    }
  }
  if (args.empty() && classDecl->library() && classDecl->name == "EventEmitter") {
    auto *events = this->eventMapInference->newEventMap(create->loc());
    args.push_back(events);
  }
  if (args.empty()) {
    return nullptr;
  }
  auto made =
      this->genericClassInstantiator->instantiateClass(classDecl, std::move(args), create->loc());
  if (made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? classDecl->loc()
            : create->loc();
    this->state.getDiagnostics().error(at, made.error().message);
  }

  return made.isOk() ? made.value()->type : nullptr;
}

}
