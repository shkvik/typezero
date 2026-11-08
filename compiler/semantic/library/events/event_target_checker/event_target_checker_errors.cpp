#include "compiler/semantic/library/events/event_target_checker/event_target_checker_errors.h"

namespace tz {

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
