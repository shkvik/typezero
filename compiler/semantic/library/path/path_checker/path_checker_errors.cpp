#include "compiler/semantic/library/path/path_checker/path_checker_errors.h"

namespace tz {

std::string ErrorTextPath::throughFns(llvm::StringRef name, llvm::StringRef module) {
  return "'" + name.str() + "' of '" + module.str() + "' is used through its functions, e.g. '" +
         name.str() + ".join(a, b)'";
}

std::string ErrorTextPath::notCalled(llvm::StringRef name) {
  return "'" + name.str() + "' is a string, not a function";
}

std::string ErrorTextPath::module(llvm::StringRef name) {
  return "'" + name.str() + "' is not a function: call its functions, e.g. '" + name.str() +
         ".join(a, b)'";
}

std::string ErrorTextPath::notYet(llvm::StringRef name, llvm::StringRef module) {
  return "'" + name.str() + "' of '" + module.str() + "' is not supported yet";
}

}
