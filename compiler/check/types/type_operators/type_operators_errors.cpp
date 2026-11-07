#include "compiler/check/types/type_operators/type_operators_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextTypeOp::keyOf(llvm::StringRef type) {
  return (llvm::Twine("'keyof' takes an object type, whose fields it names, not '") + type + "'")
      .str();
}

std::string ErrorTextTypeOp::noMember(llvm::StringRef object, llvm::StringRef key) {
  return (llvm::Twine("'") + object + "' has no member '" + key + "'").str();
}

std::string ErrorTextTypeOp::typeOf(llvm::StringRef name) {
  return (llvm::Twine("'typeof ") + name + "' needs the type of '" + name +
          "', which is not known here; declare it, e.g. 'const " + name + ": T = ...'")
      .str();
}

std::string ErrorTextTypeOp::noField(llvm::StringRef type, llvm::StringRef field) {
  return (llvm::Twine("'") + type + "' has no field '" + field + "'").str();
}

std::string ErrorTextTypeOp::rename(llvm::StringRef name) {
  return (llvm::Twine("a mapped type renames its keys to names of fields, string literal "
                      "types such as `get${Capitalize<K>}`, not '") +
          name + "'")
      .str();
}

std::string ErrorTextTypeOp::keys(llvm::StringRef key) {
  return (llvm::Twine("the keys of a mapped type are names of fields, string literal types "
                      "such as '\"id\" | \"name\"' or 'keyof T', not '") +
          key + "'")
      .str();
}

}
