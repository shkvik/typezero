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
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;

namespace tz {

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

EventMaps::EventMaps(SemanticState &owner)
    : state(owner) {}

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
  if (!events->isObject() || events->isClass() || events->isTuple() || events->isAnyObject() ||
      events->isBytes() || events->isDate() || events->isRegExp() || events->isMatch()) {
    std::string why = ErrorTextEventMap::notOne(events->name());
    std::string message = ErrorTextEventMap::notMap(why);

    return err(EventMapsEventMapFitsError{
        .code = EventMapsEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  for (const Field &f : events->fields()) {
    bool service = EventMaps::isServiceEvent(f.name);
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
    if (!service && !EventMaps::carriesOne(f.type)) {
      std::string event = ErrorTextEventMap::event(f.name);
      std::string message = ErrorTextEventMap::several(event, f.type->name());

      return err(EventMapsEventMapFitsError{
          .code = EventMapsEventMapFitsErrorCode::SEVERAL_VALUES,
          .message = message,
      });
    }
    if (service && f.type != EventMaps::serviceEventArgs()) {
      std::string message = ErrorTextEventMap::service(f.name, f.type->name());

      return err(EventMapsEventMapFitsError{
          .code = EventMapsEventMapFitsErrorCode::SERVICE_EVENT,
          .message = message,
      });
    }
  }

  return ok(events);
}

const Type *EventMaps::eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                                 std::string *label) {
  if (events->isRecord()) {
    if (auto *lit = dyn_cast<StringLitExpr>(name); lit && label) {
      *label = ToUtf8::toUtf8(lit->value);
    }
    name = ArgumentChecker(this->state).checkArg(name, Type::string());
    return name->type->isError() ? nullptr : events->valueType();
  }
  name = ArgumentChecker(this->state).checkArg(name, Type::string());
  std::optional<std::string> key = KeyedFieldChecker(this->state).constantKey(name);
  if (!key) {
    if (!name->type->isError()) {
      std::string message = ErrorTextEventName::unknown(method);
      this->state.getDiagnostics().error(name->loc(), message);
    }
    return nullptr;
  }
  if (label) {
    *label = *key;
  }
  llvm::SMLoc loc = name->loc();
  if (EventMaps::isServiceEvent(*key)) {
    return EventMaps::serviceEventArgs();
  }
  if (std::optional<std::string> site = this->state.getEventStore().findOpenMapSite(events)) {
    for (const EventInference::Event &e : this->state.getEventStore().getInferredEvents(*site)) {
      if (e.name == *key) {
        return EventMapInference(this->state).fromPortable(e.args, loc);
      }
    }
    return nullptr;
  }
  if (const Field *f = events->field(*key)) {
    return f->type;
  }
  std::string nearest = EventMaps::nearestEvent(events, *key);
  std::string message = this->state.getEventStore().isInferredMap(events)
                            ? ErrorTextEventName::notEmitted(*key, events, nearest)
                            : ErrorTextEventName::noEvent(*key, events, nearest);
  this->state.getDiagnostics().error(loc, message);

  return nullptr;
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
  const Type *sync = EventMaps::listenerType(args, Type::voidTy());
  const Type *async = EventMaps::listenerType(args, Type::promise(Type::voidTy()));
  kind = ListenerKind::Plain;
  auto takesThis = [&](const Type *self, llvm::SMLoc loc) {
    if (self->isError()) {
      return false;
    }
    ResultListenerThis taken = EventMaps::listenerThis(self, receiver);
    if (taken.isErr()) {
      this->state.getDiagnostics().error(loc, taken.error().message);
      return false;
    }

    return true;
  };
  auto withThis = [&](const Type *self) {
    std::vector<const Type *> params{self};
    params.insert(params.end(), sync->params().begin(), sync->params().end());

    return Type::function(params, sync->minArgs() + 1, sync->hasRest(), Type::voidTy(), true);
  };
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    FunctionDecl *fn = fe->fn;
    if (!fn->params.empty() && fn->params[0].name == "this" && fn->params[0].type) {
      const Type *self = TypeResolver(this->state).resolveType(*fn->params[0].type, false);
      if (!takesThis(self, fn->params[0].loc)) {
        arg = ExprChecker(this->state).checkExpr(arg);
        return arg;
      }
      if (fn->isAsync) {
        std::string message = ErrorTextListener::asyncThis();
        this->state.getDiagnostics().error(fe->loc(), message);
        arg = ExprChecker(this->state).checkExpr(arg);
        return arg;
      }
      kind = ListenerKind::This;
      const Type *wanted = withThis(self);
      Signature sig = ClosureChecker::signatureOf(wanted);
      sig.voidResult = true;
      arg = ClosureChecker(this->state).checkFunctionExpr(fe, &sig);
      if (arg->type->isError()) {
        return arg;
      }
      return TypeRelations(this->state)
          .convert(TypeRelations(this->state).coerce(arg, wanted), Type::unknown());
    }
    if (fn->isAsync) {
      kind = ListenerKind::Async;
    }
    Signature sig = ClosureChecker::signatureOf(fn->isAsync ? async : sync);
    sig.voidResult = !fn->isAsync;
    arg = ClosureChecker(this->state).checkFunctionExpr(fe, &sig);
    if (arg->type->isError()) {
      return arg;
    }
    return TypeRelations(this->state)
        .convert(TypeRelations(this->state).coerce(arg, fn->isAsync ? async : sync),
                 Type::unknown());
  }
  arg = ExprChecker(this->state).checkExpr(arg);
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
    if (!takesThis(type->params()[0], arg->loc())) {
      return arg;
    }
    kind = ListenerKind::This;
    if (type == withThis(type->params()[0])) {
      return TypeRelations(this->state).convert(arg, Type::unknown());
    }
    std::string message = ErrorTextListener::gets(event, args->name(), type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  bool isAsync = type->ret()->isPromise();
  if (isAsync) {
    kind = ListenerKind::Async;
  }
  const Type *wanted = isAsync ? async : sync;
  if (type == wanted) {
    return TypeRelations(this->state).convert(arg, Type::unknown());
  }
  if (isAsync && type->ret() != async->ret()) {
    std::string message = ErrorTextListener::asyncRet(type->ret()->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  if (Expr *adapted = FunctionValueChecker(this->state).adaptFunction(arg, wanted)) {
    return TypeRelations(this->state).convert(adapted, Type::unknown());
  }
  std::string message = ErrorTextListener::getsArrow(event, args->name(), type->name());
  this->state.getDiagnostics().error(arg->loc(), message);

  return arg;
}

const Type *EventMaps::createdType(const NewExpr *create) {
  if (!create->member.empty()) {
    return nullptr;
  }
  auto *cls = llvm::dyn_cast_or_null<ClassDecl>(TypeDeclarer(this->state).lookupType(create->name));
  if (!cls) {
    return nullptr;
  }
  if (!cls->isGeneric()) {
    return cls->type;
  }
  std::vector<const Type *> args;
  for (const TypeRef &ref : create->typeArgs) {
    args.push_back(TypeResolver(this->state).resolveType(ref, false));
    if (args.back()->isError()) {
      return nullptr;
    }
  }
  if (args.empty() && cls->library() && cls->name == "EventEmitter") {
    if (const Type *events =
            EventMapInference(this->state)
                .inferredEventMap("new " + EventMapInference(this->state).placeOf(create->loc()))) {
      args.push_back(events);
    }
  }
  if (args.empty()) {
    return nullptr;
  }
  GenericClassInstantiator::ResultClassMade made =
      GenericClassInstantiator(this->state).instantiateClass(cls, std::move(args), create->loc());
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
