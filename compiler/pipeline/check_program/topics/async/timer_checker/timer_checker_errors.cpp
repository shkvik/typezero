#include "compiler/pipeline/check_program/topics/async/timer_checker/timer_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextTaskCallback::notFunction(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'") + fn + "' needs a function, found '" + type + "'").str();
}

std::string ErrorTextTaskCallback::nothing(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'") + fn + "' calls its callback with nothing, but this function is '" +
          type + "'; pass an arrow that calls it, e.g. '() => f()'")
      .str();
}

std::string ErrorTextAsyncGlobal::callbackArg() {
  return "arguments for the callback are not supported; pass an "
         "arrow that calls it with them";
}

std::string ErrorTextTimerMethod::noMethod(llvm::StringRef name, llvm::StringRef type,
                                           bool immediate) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type +
          "'; it has 'ref', 'unref', 'hasRef'" + (immediate ? "" : " and 'refresh'"))
      .str();
}

std::string ErrorTextTimersPromises::scheduler() {
  return "'scheduler' is used through its functions: 'scheduler.wait(ms)' or "
         "'scheduler.yield()'";
}

std::string ErrorTextTimersPromises::voidValue(llvm::StringRef fn) {
  return (llvm::Twine("a value of type 'void' cannot be given to '") + fn + "'").str();
}

std::string ErrorTextScheduler::noFunction(llvm::StringRef name) {
  return (llvm::Twine("'scheduler.") + name + "' does not exist; scheduler has 'wait' and 'yield'")
      .str();
}

}
