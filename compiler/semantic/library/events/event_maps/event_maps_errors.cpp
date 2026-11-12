#include "compiler/semantic/library/events/event_maps/event_maps_errors.h"

namespace tz {

std::string ErrorTextEventMap::notMap(llvm::StringRef why) {
  return "the type argument of 'EventEmitter' is its event map: each event with the "
         "arguments its listeners get, as a tuple, e.g. 'EventEmitter<{ data: "
         "[chunk: string]; end: [] }>'; " +
         why.str();
}

std::string ErrorTextEventMap::notTuple(llvm::StringRef type) {
  return "the arguments of each event are '" + type.str() + "' here, which is not a tuple";
}

std::string ErrorTextEventMap::notOne(llvm::StringRef type) {
  return "'" + type.str() + "' is not one";
}

std::string ErrorTextEventMap::optional(llvm::StringRef event) {
  return "event '" + event.str() + "' is optional, but an event is in the map or not";
}

std::string ErrorTextEventMap::fieldTuple(llvm::StringRef event, llvm::StringRef type) {
  return "event '" + event.str() + "' has '" + type.str() + "', which is not a tuple";
}

std::string ErrorTextEventMap::eachEvent() {
  return "each event";
}

std::string ErrorTextEventMap::event(llvm::StringRef name) {
  return "event '" + name.str() + "'";
}

std::string ErrorTextEventMap::several(llvm::StringRef event, llvm::StringRef args) {
  return event.str() + " carries '" + args.str() +
         "', but an event carries one value: give it an object, e.g. "
         "'message: [message: { from: User; text: string }]' "
         "(docs/events.md, section 2)";
}

std::string ErrorTextEventMap::service(llvm::StringRef event, llvm::StringRef type) {
  return "every emitter has the event '" + event.str() +
         "', whose listeners get the name of the event and the listener, "
         "'[change: { event: string; listener: unknown }]', but the event map gives "
         "it '" +
         type.str() + "'; leave it out of the map";
}

std::string ErrorTextEventName::unknown(llvm::StringRef method) {
  return "the name of the event must be known when compiling, as a "
         "string literal, e.g. '" +
         method.str() +
         "(\"data\", ...)'; to "
         "name events known only when the program runs, give the "
         "emitter a map that takes any name, e.g. 'new EventEmitter<{ "
         "[name: string]: [msg: string] }>()'";
}

std::string ErrorTextEventName::list(const Type *events) {
  auto out = std::string();
  llvm::ArrayRef<Field> fields = events->fields();
  for (size_t i = 0; i < fields.size(); ++i) {
    out += (i == 0 ? "" : i + 1 == fields.size() ? " and " : ", ") + ("'" + fields[i].name + "'");
  }

  return out;
}

std::string ErrorTextEventName::didYouMean(llvm::StringRef name) {
  return name.empty() ? std::string() : "; did you mean '" + name.str() + "'?";
}

std::string ErrorTextEventName::notEmitted(llvm::StringRef key, const Type *events,
                                           llvm::StringRef nearest) {
  std::string message = "no code of the program emits '" + key.str() + "' on this emitter";
  if (events->fields().empty()) {
    message += ", nor any other event: an emitter without an event map has the events that "
               "emit() calls give it, e.g. 'bus.emit(\"" +
               key.str() + "\", ...)'";
  } else {
    message += "; it emits " + ErrorTextEventName::list(events);
  }

  return message + ErrorTextEventName::didYouMean(nearest);
}

std::string ErrorTextEventName::untyped(llvm::StringRef key, llvm::StringRef place) {
  return "the type of '" + key.str() + "' comes from the emit at " + place.str() +
         ", which is checked later and uses names not known here; move that emit into a "
         "function, or give the emitter its event map, e.g. 'new EventEmitter<{ " +
         key.str() + ": [value: string] }>()' (docs/events.md, section 2)";
}

std::string ErrorTextEventName::circular(llvm::StringRef key) {
  return "the type of '" + key.str() + "' is needed here while it is still being found from " +
         "the emits that this code leads to; give the emitter its event map, e.g. " +
         "'new EventEmitter<{ " + key.str() + ": [value: string] }>()' (docs/events.md, section 2)";
}

std::string ErrorTextEventName::noEvent(llvm::StringRef key, const Type *events,
                                        llvm::StringRef nearest) {
  std::string message = "the emitter has no event '" + key.str() + "'";
  if (events->fields().empty()) {
    message += ": its event map has none";
  } else {
    message += ": its event map has " + ErrorTextEventName::list(events);
  }

  return message + ErrorTextEventName::didYouMean(nearest);
}

std::string ErrorTextListener::thisType(llvm::StringRef self, llvm::StringRef receiver) {
  return "the listener takes 'this' of type '" + self.str() +
         "', but it gets the emitter, of type '" + receiver.str() + "'";
}

std::string ErrorTextListener::asyncThis() {
  return "an async listener cannot take 'this'; use the emitter by name";
}

std::string ErrorTextListener::notFunc(llvm::StringRef type) {
  return "a listener is a function, but this is '" + type.str() + "'";
}

std::string ErrorTextListener::gets(llvm::StringRef event, llvm::StringRef args,
                                    llvm::StringRef type) {
  return "the listeners of '" + event.str() + "' get '" + args.str() + "', but this function is '" +
         type.str() + "'";
}

std::string ErrorTextListener::getsArrow(llvm::StringRef event, llvm::StringRef args,
                                         llvm::StringRef type) {
  return ErrorTextListener::gets(event, args, type) +
         "; pass an arrow that calls it with what it needs";
}

std::string ErrorTextListener::asyncRet(llvm::StringRef type) {
  return "an async listener returns 'Promise<void>', but this one returns '" + type.str() +
         "', which the emitter would drop; wrap it, "
         "e.g. 'async (...) => { await f(...) }'";
}

}
