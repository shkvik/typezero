#include "compiler/semantic/types_v2/typeof/types_typeof_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextTypeOf::typeOf(llvm::StringRef name) {
  return (llvm::Twine("'typeof ") + name + "' needs the type of '" + name +
          "', which is not known here; declare it, e.g. 'const " + name + ": T = ...'")
      .str();
}

std::string ErrorTextTypeOf::noField(llvm::StringRef type, llvm::StringRef field) {
  return (llvm::Twine("'") + type + "' has no field '" + field + "'").str();
}

}
