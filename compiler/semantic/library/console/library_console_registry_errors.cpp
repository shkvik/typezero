#include "compiler/semantic/library/console/library_console_registry_errors.h"

namespace tz {

std::string ErrorTextConsole::voidValue() {
  return "an expression of type 'void' cannot be printed";
}

std::string ErrorTextConsole::spread(llvm::StringRef type) {
  return "only an array can be spread into console.log, found '" + type.str() + "'";
}

std::string ErrorTextConsole::notYet(llvm::StringRef name) {
  return "'console." + name.str() +
         "' is not supported yet; only 'console.log', 'info', "
         "'debug', 'error' and 'warn' are";
}

}
