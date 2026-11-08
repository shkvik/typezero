#include "compiler/interprocedural/held_value_checker/held_value_checker_errors.h"

namespace tz {

std::string ErrorTextHeldPlace::moduleVar() {
  return "in a module variable";
}

std::string ErrorTextHeldPlace::newObject() {
  return "in a new object";
}

std::string ErrorTextHeldPlace::array() {
  return "in an array";
}

std::string ErrorTextHeldPlace::unknownFn() {
  return "by a function the compiler can't follow";
}

std::string ErrorTextHeldPlace::storedBy(llvm::StringRef name) {
  return "by '" + name.str() + "', which may store it";
}

std::string ErrorTextHeldPlace::outerVar() {
  return "in a variable of an outer function";
}

std::string ErrorTextHeldPlace::container() {
  return "in an array or a map";
}

std::string ErrorTextHeldPlace::field() {
  return "in a field";
}

}
