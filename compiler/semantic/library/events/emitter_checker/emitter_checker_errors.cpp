#include "compiler/semantic/library/events/emitter_checker/emitter_checker_errors.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker_errors.h"

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
  std::string out;
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

}
