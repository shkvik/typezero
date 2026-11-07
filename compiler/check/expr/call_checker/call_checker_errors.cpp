#include "compiler/check/expr/call_checker/call_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextValueCall::takesThis() {
  return "the function takes its 'this' first, which a plain call "
         "does not pass; call it with '.call(obj, ...)' or "
         "'.apply(obj, args)'";
}

std::string ErrorTextCallTypeArgs::forGeneric() {
  return "type arguments can be given only to a generic function";
}

std::string ErrorTextCallTypeArgs::notGeneric(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is not a generic function; remove the type arguments").str();
}

std::string ErrorTextCallee::undefined() {
  return "the function may be undefined; check it first, or call it with "
         "'?.()', which gives undefined then";
}

std::string ErrorTextCallee::notFunc(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' is not a function; it has type '" + type + "'").str();
}

std::string ErrorTextCallee::uncallable(llvm::StringRef type) {
  return (llvm::Twine("a value of type '") + type + "' cannot be called").str();
}

std::string ErrorTextSpread::notRest() {
  return "a spread argument can only be passed to a rest parameter, "
         "Math.min, Math.max or push";
}

std::string ErrorTextCallMethod::undefined(llvm::StringRef name) {
  return (llvm::Twine("the value may be undefined; check it before calling '") + name + "'").str();
}

std::string ErrorTextCallMethod::setsThis(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is not supported: it sets 'this', and a "
          "method taken as a value is bound to its "
          "object already; call the function "
          "directly")
      .str();
}

std::string ErrorTextCallMethod::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextCallName::noSupport(llvm::StringRef name, llvm::StringRef why) {
  return (llvm::Twine("'") + name + "' is not supported: " + why).str();
}

std::string ErrorTextCallName::notFunc(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is not a function").str();
}

}
