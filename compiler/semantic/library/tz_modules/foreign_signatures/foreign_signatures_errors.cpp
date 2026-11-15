#include "compiler/semantic/library/tz_modules/foreign_signatures/foreign_signatures_errors.h"

namespace tz {

std::string ErrorTextForeign::plain() {
  return "a function in C takes plain parameters: no type parameters, no rest";
}

std::string ErrorTextForeign::optional() {
  return "a function in C has no optional parameters";
}

std::string ErrorTextForeign::takes(llvm::StringRef type) {
  return "a function in C cannot take a value of type '" + type.str() +
         "'; it takes numbers, 'boolean', 'cstring', handles ('declare class'), arrays of numbers "
         "and handles, 'Buffer' and 'Out<T>'";
}

std::string ErrorTextForeign::gives(llvm::StringRef type) {
  return "a function in C cannot give a value of type '" + type.str() +
         "'; it gives numbers, 'boolean', 'cstring' and handles ('declare class')";
}

std::string ErrorTextForeign::noReturn() {
  return "a function in C needs its return type written: tz cannot see its code";
}

}
