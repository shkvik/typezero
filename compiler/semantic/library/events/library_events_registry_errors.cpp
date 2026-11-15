#include "compiler/semantic/library/events/library_events_registry_errors.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"

namespace tz {

std::string ErrorTextListenerLife::what(bool timer) {
  return timer ? "interval" : "listener";
}

std::string ErrorTextListenerLife::ends(bool timer) {
  return timer ? "it should stop" : "its subscription ends";
}

std::string ErrorTextListenerLife::section(bool timer) {
  return timer ? "(docs/events.md, section 7)" : "(docs/events.md, section 6)";
}

std::string ErrorTextListenerLife::list(const std::vector<const VarSymbol *> &vars) {
  auto out = std::string();
  for (size_t i = 0; i < vars.size(); ++i) {
    out += (i == 0                 ? ""
            : i + 1 == vars.size() ? " and "
                                   : ", ") +
           ("'" + vars[i]->name.str() + "'");
  }

  return out;
}

std::string ErrorTextListenerLife::unknown(bool timer) {
  return "can't tell what this " + ErrorTextListenerLife::what(timer) + " captures, so not when " +
         ErrorTextListenerLife::ends(timer) +
         "; pass a method here, e.g. 'this.onData', or an "
         "arrow that uses one object, e.g. '(chunk) => "
         "this.add(chunk)' " +
         ErrorTextListenerLife::section(timer);
}

std::string ErrorTextListenerLife::keeps(bool timer, llvm::StringRef name) {
  std::string example = timer ? "pass its method, e.g. 'setInterval(counter.tick, 1000)'"
                              : "subscribe its method, e.g. 'bus.on(\"data\", counter.add)'";

  return "the " + ErrorTextListenerLife::what(timer) + " keeps '" + name.str() +
         "', which nothing else holds, so nothing tells when " +
         ErrorTextListenerLife::ends(timer) + "; keep the state in an object and " + example + " " +
         ErrorTextListenerLife::section(timer);
}

std::string ErrorTextListenerLife::captures(bool timer,
                                            const std::vector<const VarSymbol *> &captured,
                                            const std::vector<const VarSymbol *> &others,
                                            llvm::StringRef kept) {
  return "the " + ErrorTextListenerLife::what(timer) + " captures " +
         ErrorTextListenerLife::list(captured) +
         ", but it may depend on one object only, the one whose "
         "life it follows; store what it needs from " +
         ErrorTextListenerLife::list(others) + " in a field of '" + kept.str() + "' " +
         ErrorTextListenerLife::section(timer);
}

std::string ErrorTextUnlistened::mayEmit(llvm::StringRef path) {
  std::string name = path.empty() ? std::string("this emitter") : "'" + path.str() + "'";
  std::string shown = path.empty() ? std::string("s") : path.str();

  return name +
         " may emit 'error', and nothing listens for it on every path "
         "here, so the error would be lost (node would end the "
         "program); listen in this function: '" +
         shown + ".on(\"error\", (e) => ...)'";
}

std::string ErrorTextUnlistened::unheard() {
  return "'error' is emitted here, but nothing in the program listens for "
         "'error' on this emitter, so the error would be lost (node "
         "would end the program); listen for it, e.g. "
         "'bus.on(\"error\", (e) => ...)'";
}

std::string ErrorTextEmitterNew::dropped(llvm::StringRef cls) {
  return "the new " + cls.str() +
         " is held by nothing, so the listeners its constructor "
         "adds would stop at once; store it, e.g. 'const " +
         cls.lower() + " = new " + cls.str() + "(...)' (docs/events.md, section 6)";
}

std::string ErrorTextEmitted::oneValue(llvm::StringRef event) {
  return "an event carries one value: pass an object, e.g. 'emit(\"" + event.str() +
         "\", { from, text })' (docs/events.md, section 2)";
}

std::string ErrorTextEmitted::noName() {
  return "'emit' needs the name of the event, e.g. 'emit(\"data\", ...)'";
}

std::string ErrorTextEmitted::typeArgs(llvm::StringRef name) {
  return "'" + name.str() +
         "' takes no type arguments: the event map of "
         "the emitter gives the types";
}

std::string ErrorTextEmitted::countAt(llvm::StringRef event, llvm::StringRef args,
                                      llvm::StringRef first, size_t count) {
  return "'" + event.str() + "' is emitted with " + args.str() + " at " + first.str() +
         ", but with " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") + " here";
}

std::string ErrorTextEmitted::arity(llvm::StringRef event, llvm::StringRef args, size_t min,
                                    size_t max, size_t count) {
  return "the listeners of '" + event.str() + "' get '" + args.str() +
         "': " + ErrorTextArity::wanted(unsigned(min), unsigned(max)) + ", but got " +
         std::to_string(count);
}

std::string ErrorTextEmitted::typeAt(llvm::StringRef event, llvm::StringRef args,
                                     llvm::StringRef first, llvm::StringRef type) {
  return "'" + event.str() + "' is emitted with " + args.str() + " at " + first.str() +
         ", but with '" + type.str() + "' here; an event has one type of arguments";
}

std::string ErrorTextEmitted::methodVal(llvm::StringRef name) {
  return "'" + name.str() +
         "' of an emitter can only be called: the event "
         "map checks each call by the name of its event; wrap it in an "
         "arrow, e.g. '(listener) => bus." +
         name.str() + "(\"data\", listener)'";
}

std::string ErrorTextEmitted::foreign(llvm::StringRef event, llvm::StringRef owner) {
  return "'" + event.str() + "' is not an event of this emitter: its events are the ones " +
         "emitted in " + owner.str() + ", where it is made, and other modules emit only those; " +
         "emit '" + event.str() + "' in " + owner.str() +
         ", e.g. in a method of its class, or give the emitter its event map (docs/events.md, "
         "section 2)";
}

std::string ErrorTextEmitted::voidValue(llvm::StringRef event) {
  return "this has no value to send with '" + event.str() + "': it is 'void'; emit the event " +
         "without a value, e.g. 'emit(\"" + event.str() + "\")'";
}

std::string ErrorTextEventsExport::hint(llvm::StringRef name) {
  if (name == "errorMonitor" || name == "captureRejectionSymbol") {
    return ": in node it is a symbol, and tz has no symbols of its own; the names of events are "
           "strings (docs/events.md, section 4)";
  }
  if (name == "addAbortListener") {
    return " yet; listen to the signal: 'signal.addEventListener(\"abort\", listener, { once: "
           "true })'";
  }
  if (name == "EventEmitterAsyncResource" || name == "usingDomains") {
    return ": tz has neither async resources nor domains; a store that follows a request is "
           "AsyncLocalStorage of \"node:async_hooks\"";
  }
  if (name == "EventTarget" || name == "Event" || name == "CustomEvent") {
    return "; '" + name.str() + "' is global, as in node: use it without an import";
  }

  return "";
}

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

std::string ErrorTextEventTarget::listener(llvm::StringRef type) {
  return "a listener of an EventTarget takes an Event, or a kind of it such as "
         "'CustomEvent<int>', but this one takes '" +
         type.str() + "'";
}

std::string ErrorTextEventsFunction::notEmitter(llvm::StringRef fn, bool target,
                                                llvm::StringRef type) {
  return "'" + fn.str() + "' takes an emitter of node:events" +
         (target ? " or an EventTarget" : "") + ", but this is '" + type.str() + "'";
}

std::string ErrorTextEventsFunction::targetOn() {
  return "'on' of node:events takes an emitter of node:events; "
         "the events of an EventTarget are not supported yet: "
         "listen with 'addEventListener', or wait for one with "
         "'await once(target, name)'";
}

std::string ErrorTextEventsFunction::endless(llvm::StringRef name, llvm::StringRef fn) {
  std::string emitter = name.empty() ? std::string("this emitter") : "'" + name.str() + "'";

  return emitter +
         " may never emit this event, so the wait may never end; give "
         "it a time limit, e.g. '" +
         fn.str() +
         "(emitter, \"done\", { timeout: 5000 })' (docs/events.md, "
         "section 6)";
}

std::string ErrorTextEventsFunction::options() {
  return "the options of 'once' have only 'timeout', e.g. '{ timeout: 5000 }'";
}

std::string ErrorTextEventsFunction::errorValue(llvm::StringRef fn, llvm::StringRef errors) {
  return "'" + fn.str() +
         "' gives the error of the emitter as the Err, so its 'error' "
         "event carries one value; this one carries '" +
         errors.str() + "'";
}

}
