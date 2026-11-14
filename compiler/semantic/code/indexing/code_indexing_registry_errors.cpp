#include "compiler/semantic/code/indexing/code_indexing_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextIndexRead::undefined() {
  return "the value may be undefined; check it before indexing";
}

std::string ErrorTextIndexRead::notIndexed(llvm::StringRef type) {
  return (llvm::Twine("type '") + type + "' cannot be indexed").str();
}

std::string ErrorTextPosition::notInt(llvm::StringRef type, bool fraction, bool undefined) {
  std::string message =
      (llvm::Twine("an index or position must be an integer, found '") + type + "'").str();
  if (fraction) {
    message += "; round it with Math.floor(x) or drop the fraction with 'x | 0'";
  } else if (undefined) {
    message += "; the value may be undefined: check it first or give a default with '??'";
  }

  return message;
}

std::string ErrorTextGenericIndex::notByKey(llvm::StringRef object, llvm::StringRef key) {
  return (llvm::Twine("'") + object +
          "' is indexed by its keys, such as 'k: K' with 'K "
          "extends keyof " +
          object + "', not by '" + key + "'")
      .str();
}

std::string ErrorTextEnumIndex::strings(llvm::StringRef name) {
  return (llvm::Twine("enum '") + name + "' has string values, and only numbers map back to names")
      .str();
}

std::string ErrorTextEnumIndex::notInteger(llvm::StringRef type) {
  return (llvm::Twine("the value of an enum member is an integer, found '") + type + "'").str();
}

}
