#include "compiler/semantic/library_v2/events/library_emitters.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/events/emitter_event_table/emitter_event_table.h"
#include "compiler/program/library/events/listener_change_field_table/listener_change_field_table.h"
#include "compiler/program/types/type_queries/type_queries.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <algorithm>

namespace tz {

LibraryEmitters::LibraryEmitters(SemanticState &owner)
    : state(owner) {}

bool LibraryEmitters::isServiceEvent(llvm::StringRef name) const {
  auto event = EmitterEventTable::find(name);

  return event == EmitterEvent::NEW_LISTENER || event == EmitterEvent::REMOVE_LISTENER;
}

const Type *LibraryEmitters::serviceEventArgs() const {
  auto eventName = ListenerChangeFieldTable::getName(ListenerChangeField::EVENT);
  auto listenerName = ListenerChangeFieldTable::getName(ListenerChangeField::LISTENER);
  const auto *change = Type::object(
      {{eventName.str(), Type::string(), false}, {listenerName.str(), Type::unknown(), false}});

  return Type::tuple({change});
}

bool LibraryEmitters::isCarryingOne(const Type *args) const {
  return args->fields().size() <= 1 && args->tupleRest() == nullptr;
}

std::string LibraryEmitters::nearestEvent(const Type *events, llvm::StringRef name) const {
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

bool LibraryEmitters::isEmitterClass(const ClassDecl *classDecl) const {
  auto *base = this->state.library.declarations.findLibraryClass(LibraryClass::EVENT_EMITTER_BASE);

  return classDecl != nullptr && base != nullptr && classDecl->derivesFrom(base);
}

const Type *LibraryEmitters::eventMapOf(const Type *type) const {
  const auto *emitter =
      this->state.library.declarations.findLibraryClass(LibraryClass::EVENT_EMITTER);
  if (emitter == nullptr) {
    return nullptr;
  }
  const auto *streams =
      this->state.library.declarations.findLibraryClass(LibraryClass::STREAM_EMITTER);
  for (const ClassDecl *classDecl = type->classDecl(); classDecl != nullptr;
       classDecl = classDecl->parent) {
    if (classDecl->generic() != nullptr &&
        (classDecl->generic() == emitter || classDecl->generic() == streams) &&
        !classDecl->typeArgs.empty()) {
      return classDecl->typeArgs[0];
    }
  }

  return nullptr;
}

LibraryEmitters::ResultEventMapFits LibraryEmitters::checkEventMapFits(const Type *events) const {
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

    return err(LibraryEmittersEventMapFitsError{
        .code = LibraryEmittersEventMapFitsErrorCode::NOT_A_MAP,
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

LibraryEmitters::ResultEventMapFits LibraryEmitters::checkRecordMapFits(const Type *events) const {
  const Type *args = events->valueType();
  if (!args->isTuple()) {
    auto why = ErrorTextEventMap::notTuple(args->name());
    auto message = ErrorTextEventMap::notMap(why);

    return err(LibraryEmittersEventMapFitsError{
        .code = LibraryEmittersEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!this->isCarryingOne(args)) {
    auto event = ErrorTextEventMap::eachEvent();
    auto message = ErrorTextEventMap::several(event, args->name());

    return err(LibraryEmittersEventMapFitsError{
        .code = LibraryEmittersEventMapFitsErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }

  return ok(events);
}

LibraryEmitters::ResultEventMapFits LibraryEmitters::checkEventFieldFits(const Field &field) const {
  bool isService = this->isServiceEvent(field.name);
  if (field.type->isOptional()) {
    auto why = ErrorTextEventMap::optional(field.name);
    auto message = ErrorTextEventMap::notMap(why);

    return err(LibraryEmittersEventMapFitsError{
        .code = LibraryEmittersEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!field.type->isTuple()) {
    auto why = ErrorTextEventMap::fieldTuple(field.name, field.type->name());
    auto message = ErrorTextEventMap::notMap(why);

    return err(LibraryEmittersEventMapFitsError{
        .code = LibraryEmittersEventMapFitsErrorCode::NOT_A_MAP,
        .message = message,
    });
  }
  if (!isService && !this->isCarryingOne(field.type)) {
    auto event = ErrorTextEventMap::event(field.name);
    auto message = ErrorTextEventMap::several(event, field.type->name());

    return err(LibraryEmittersEventMapFitsError{
        .code = LibraryEmittersEventMapFitsErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }
  if (isService && field.type != this->serviceEventArgs()) {
    auto message = ErrorTextEventMap::service(field.name, field.type->name());

    return err(LibraryEmittersEventMapFitsError{
        .code = LibraryEmittersEventMapFitsErrorCode::SERVICE_EVENT,
        .message = message,
    });
  }

  return ok(field.type);
}

const Type *LibraryEmitters::listenerType(const Type *args, const Type *returnType) const {
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

}
