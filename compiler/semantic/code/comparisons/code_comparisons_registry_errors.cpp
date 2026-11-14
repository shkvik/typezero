#include "compiler/semantic/code/comparisons/code_comparisons_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextTypeOfName::noNumber(llvm::StringRef here) {
  return (llvm::Twine("typeof gives no \"number\": each number type has its own name, "
                      "\"int\", \"double\", \"decimal\", \"int32\"...; the type 'number' is "
                      "'double', and its typeof is \"double\"") +
          here)
      .str();
}

std::string ErrorTextTypeOfName::forType(llvm::StringRef type, llvm::StringRef names) {
  return (llvm::Twine("; for a value of type '") + type + "' it is " + names).str();
}

std::string ErrorTextTypeOfName::noSymbol(llvm::StringRef names) {
  return (llvm::Twine("typeof gives no \"symbol\": tz has no symbols as values; the names are ") +
          names)
      .str();
}

std::string ErrorTextTypeOfName::notName(llvm::StringRef name, llvm::StringRef names) {
  return (llvm::Twine("'") + name + "' is not a name that typeof gives; the names are " + names)
      .str();
}

std::string ErrorTextTypeOfName::never(llvm::StringRef type, llvm::StringRef names,
                                       llvm::StringRef name) {
  return (llvm::Twine("typeof of a value of type '") + type + "' is " + names + ", never \"" +
          name + "\"")
      .str();
}

std::string ErrorTextTypeOfName::bigint() {
  return "'typeof x == \"bigint\"'";
}

std::string ErrorTextResultEquality::compared(llvm::StringRef op) {
  return (llvm::Twine("Results cannot be compared with '") + op +
          "': in JavaScript 'ok(1) == ok(1)' is false, as they are two "
          "objects; compare what they hold, with 'isOk()', 'match' or "
          "'value'")
      .str();
}

std::string ErrorTextIn::undefined() {
  return "the value may be undefined, and 'in' would throw then; check "
         "it first";
}

std::string ErrorTextIn::byLiteral() {
  return "operator 'in' tells the objects of a union apart by a field, "
         "written as a string literal: '\"radius\" in shape'";
}

std::string ErrorTextIn::maybeUndef(llvm::StringRef name, llvm::StringRef member,
                                    llvm::StringRef text) {
  return (llvm::Twine("operator 'in' cannot test field '") + name + "' of '" + member +
          "', which may hold undefined, as one that is "
          "not there; check the value instead: '" +
          text + " != undefined'")
      .str();
}

std::string ErrorTextIn::always(bool with, llvm::StringRef unionName, llvm::StringRef name) {
  return (llvm::Twine("operator 'in' is always ") + (with ? "true" : "false") + " here: " +
          (with ? "every" : "no") + " member of '" + unionName + "' has field '" + name + "'")
      .str();
}

std::string ErrorTextIn::tuple() {
  return "operator 'in' is not supported for tuples: a tuple always has all its elements";
}

std::string ErrorTextIn::objMaybe(llvm::StringRef text) {
  return (llvm::Twine("operator 'in' is not supported for objects: a field that is not there is "
                      "undefined; check it with '") +
          text + " != undefined'")
      .str();
}

std::string ErrorTextIn::objTrue() {
  return "operator 'in' is not supported for objects: an object always has the "
         "fields of its type, so this would always be true";
}

std::string ErrorTextIn::objFalse(llvm::StringRef type, llvm::StringRef name) {
  return (llvm::Twine("operator 'in' is not supported for objects: type '") + type +
          "' has no field '" + name + "', so this would always be false")
      .str();
}

std::string ErrorTextIn::objPlain() {
  return "operator 'in' is not supported for objects: an object always has the "
         "fields of its type";
}

std::string ErrorTextIn::array(llvm::StringRef check) {
  return (llvm::Twine("operator 'in' is not supported for arrays, which have no holes; compare "
                      "the index with the length instead: '") +
          check + "'")
      .str();
}

}
