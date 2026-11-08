#include "compiler/semantic/bodies/objects/record_literal_checker/record_literal_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextRecordProperty::defaulted(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          " = ...' gives a default value, which only a "
          "destructuring pattern has; in an object literal "
          "write '" +
          name + ": ...'")
      .str();
}

std::string ErrorTextRecordProperty::method(llvm::StringRef name) {
  return (llvm::Twine("a Record holds values, not methods; store a function, '") + name +
          ": (...) => ...'")
      .str();
}

std::string ErrorTextRecordProperty::keyLabel(llvm::StringRef name) {
  return (llvm::Twine("key '") + name + "'").str();
}

std::string ErrorTextRecordProperty::valueLabel() {
  return "the value";
}

std::string ErrorTextRecordSpread::undefined() {
  return "the object may be undefined; check it before spreading it";
}

std::string ErrorTextRecordSpread::keyedRest() {
  return "the rest of a pattern that reads a key in brackets is not supported "
         "for a Record; name the keys it leaves out";
}

std::string ErrorTextRecordSpread::values(llvm::StringRef type, llvm::StringRef value) {
  return (llvm::Twine("the values of '") + type + "' are not '" + value + "'").str();
}

std::string ErrorTextRecordSpread::field(llvm::StringRef field, llvm::StringRef type,
                                         llvm::StringRef fieldType, llvm::StringRef value) {
  return (llvm::Twine("field '") + field + "' of '" + type + "' is '" + fieldType +
          "', and the values here are '" + value + "'")
      .str();
}

std::string ErrorTextRecordSpread::methods() {
  return "an object with methods cannot be spread into a Record; copy "
         "its fields by name";
}

std::string ErrorTextRecordSpread::notObject(llvm::StringRef type, bool collection) {
  return (llvm::Twine("only objects and Records can be spread into a Record, found '") + type +
          "'" +
          (collection ? "; in JavaScript a Map or a Set has no own properties, so this would "
                        "add nothing"
                      : ""))
      .str();
}

std::string ErrorTextRecordValues::differ(llvm::StringRef value, llvm::StringRef other) {
  return (llvm::Twine("spreading a Record makes a Record, whose values have one type; found '") +
          value + "' and '" + other + "'; declare its type, e.g. 'const r: Record<string, " +
          value + "> = { ... }'")
      .str();
}

std::string ErrorTextRecordValue::undefined() {
  return "cannot infer the type of the values from 'undefined'; declare it, "
         "e.g. 'const r: Record<string, int | undefined> = { ... }'";
}

}
