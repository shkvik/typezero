#include "compiler/pipeline/check_program/topics/objects/keyed_field_checker/keyed_field_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextKeyedField::tupleKey(llvm::StringRef key) {
  return (llvm::Twine("a tuple is indexed by a number, found '") + key + "'").str();
}

std::string ErrorTextKeyedField::tupleConst() {
  return "a tuple can be indexed only by a constant, such as '[0]': each element has "
         "its own type; or destructure it: 'const [a, b] = t'";
}

std::string ErrorTextKeyedField::tupleRange(llvm::StringRef tuple, size_t count, int64_t position) {
  return (llvm::Twine("tuple '") + tuple + "' has " + std::to_string(count) + " element" +
          (count == 1 ? "" : "s") + "; there is no element " + std::to_string(position))
      .str();
}

std::string ErrorTextKeyedField::readonly() {
  return "a readonly field cannot be assigned through a key";
}

std::string ErrorTextKeyedField::byName(llvm::StringRef field) {
  return (llvm::Twine("fields of type '") + field +
          "' are assigned by name, as in 'o.name = v'; through a "
          "key of literals only fields of a plain type are, or "
          "fields named by the members of a string enum")
      .str();
}

std::string ErrorTextKeyedField::numbered(llvm::StringRef type) {
  return (llvm::Twine("type '") + type +
          "' has no numbered fields; read a field by its "
          "name, e.g. 'o.name'")
      .str();
}

std::string ErrorTextKeyedField::runtimeKey(llvm::StringRef type) {
  return (llvm::Twine("type '") + type +
          "' can be indexed only by a key known when compiling: a string literal, "
          "or a constant declared with one, such as 'const key = \"name\"'; "
          "keys computed when the program runs need a dictionary: declare it as one, e.g. "
          "'const d: Record<string, int> = {}'")
      .str();
}

std::string ErrorTextKeyedField::keyString(llvm::StringRef key) {
  return (llvm::Twine("a key must be a string, found '") + key + "'").str();
}

std::string ErrorTextKeyedField::noField(llvm::StringRef name, llvm::StringRef type,
                                         bool inherited) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'" +
          (inherited ? "; objects inherit nothing from Object.prototype" : ""))
      .str();
}

std::string ErrorTextKeyedField::tupleSet() {
  return "an element of a tuple is assigned by its number: 't[0] = ...'";
}

std::string ErrorTextKeyedField::keySet() {
  return "a field assigned through a key in brackets must be "
         "named by a string literal, or a constant declared with "
         "one, such as 'const key = \"name\"'";
}

}
