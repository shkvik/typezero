#include "compiler/semantic/library/os/library_os_registry_errors.h"

namespace tz {

std::string ErrorTextOs::notCalled(llvm::StringRef name) {
  return "'" + name.str() + "' is a string, not a function";
}

std::string ErrorTextOs::notYet(llvm::StringRef name) {
  return "'" + name.str() + "' of 'node:os' is not supported yet";
}

}
