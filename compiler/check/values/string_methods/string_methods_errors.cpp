#include "compiler/check/values/string_methods/string_methods_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextStringMethod::missing(llvm::StringRef name) {
  return (llvm::Twine("property '") + name + "' does not exist on type 'string'").str();
}

}
