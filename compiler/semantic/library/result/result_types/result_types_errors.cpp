#include "compiler/semantic/library/result/result_types/result_types_errors.h"

namespace tz {

std::string ErrorTextResultType::neverthrow() {
  return "Result, ok and err are built in: remove the import from 'neverthrow'";
}

std::string ErrorTextResultType::typeArgs(llvm::StringRef name) {
  return "'" + name.str() + "' takes two type arguments, the value and the error, e.g. '" +
         name.str() + "<int, string>'";
}

}
