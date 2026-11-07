#include "compiler/pipeline/check_program/types/utility_types/utility_types_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextUtility::count(llvm::StringRef name, int count) {
  return (llvm::Twine("'") + name + "' takes " + std::to_string(count) + " type argument" +
          (count == 1 ? "" : "s"))
      .str();
}

std::string ErrorTextUtility::noFunction(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' takes a function type, not '" + type + "'").str();
}

std::string ErrorTextUtility::notString(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name +
          "' takes a string type, such as a string literal type, "
          "not '" +
          type + "'")
      .str();
}

std::string ErrorTextGenericIndex::notByKey(llvm::StringRef object, llvm::StringRef key) {
  return (llvm::Twine("'") + object +
          "' is indexed by its keys, such as 'k: K' with 'K "
          "extends keyof " +
          object + "', not by '" + key + "'")
      .str();
}

}
