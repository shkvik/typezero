#include "compiler/semantic/library/tz_modules/library_calls/library_calls_errors.h"

namespace tz {

std::string ErrorTextLibraryCall::listener() {
  return "'asListener' gives a function";
}

std::string ErrorTextLibraryCall::asClass() {
  return "'asClass' takes a class";
}

std::string ErrorTextLibraryCall::ahead(llvm::StringRef name) {
  return "'" + name.str() +
         "' takes what an await waits for: a promise, or a "
         "value or a promise";
}

std::string ErrorTextLibraryCall::context() {
  return "'withContext' takes a function without parameters";
}

}
