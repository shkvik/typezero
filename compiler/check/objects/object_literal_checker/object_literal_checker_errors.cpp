#include "compiler/check/objects/object_literal_checker/object_literal_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextObjectLiteral::defaulted(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          " = ...' gives a default value, which only a destructuring "
          "pattern such as '({ " +
          name + " = 1 } = o)' has; in an object literal write '" + name + ": ...'")
      .str();
}

std::string ErrorTextObjectLiteral::numberKey() {
  return "a number cannot name a field; an object with numbered entries is an array";
}

std::string ErrorTextObjectLiteral::runtimeKey() {
  return "a key in brackets must be known when compiling: a string "
         "literal, or a constant declared with one, such as 'const key = "
         "\"name\"'; "
         "keys computed when the program runs need a dictionary: declare it as one, e.g. "
         "'const d: Record<string, int> = {}'";
}

std::string ErrorTextObjectLiteral::keyType(llvm::StringRef type) {
  return (llvm::Twine("a key must be a string, found '") + type + "'").str();
}

std::string ErrorTextObjectLiteral::duplicate(llvm::StringRef name) {
  return (llvm::Twine("duplicate field '") + name + "' in the object literal").str();
}

std::string ErrorTextObjectLiteral::inferFrom(llvm::StringRef type) {
  return (llvm::Twine("cannot infer the type of the fields from '") + type +
          "'; declare the type of the object, e.g. 'const o: { a?: int "
          "} = ...'")
      .str();
}

std::string ErrorTextObjectLiteral::keyTypes(llvm::StringRef name) {
  return (llvm::Twine("field '") + name +
          "' gets values of different types; declare "
          "the type of the object")
      .str();
}

std::string ErrorTextObjectLiteral::keySpread(llvm::StringRef type) {
  return (llvm::Twine("a key of type '") + type +
          "' in brackets works in an object literal without '...' "
          "and methods; assign it after: 'o[key] = value'")
      .str();
}

std::string ErrorTextObjectLiteral::keyNoType(llvm::StringRef type) {
  return (llvm::Twine("a key of type '") + type +
          "' in brackets needs the type of the object, which says "
          "the fields it may name; write it, e.g. 'const o: T = { [key]: "
          "value }'")
      .str();
}

std::string ErrorTextObjectLiteral::keyFields(llvm::StringRef type, llvm::StringRef target) {
  return (llvm::Twine("the fields that '") + type + "' names must all be in '" + target +
          "' and have one type")
      .str();
}

std::string ErrorTextObjectLiteral::keyedOpt(llvm::StringRef type, llvm::StringRef field) {
  return (llvm::Twine("the fields that '") + type + "' names are of type '" + field +
          "', not optional; the object gets only one of them, so the "
          "others would be missing")
      .str();
}

std::string ErrorTextObjectLiteral::noField(llvm::StringRef target, llvm::StringRef name,
                                            bool stream) {
  std::string message = (llvm::Twine("type '") + target + "' has no field '" + name +
                         "'; an object must have exactly the fields of its type")
                            .str();
  if (stream) {
    message += "; an error with fields of its own has a type of its own, e.g. 'interface "
               "MyError extends StreamError { ... }', and the stream names it: "
               "'new ReadableStream<int, MyError>(...)', 'class Lines extends "
               "Readable<string, MyError>'";
  }

  return message;
}

std::string ErrorTextObjectLiteral::voidValue(llvm::StringRef what) {
  return (llvm::Twine("an expression of type 'void' cannot be the value of ") + what).str();
}

std::string ErrorTextObjectLiteral::undefOf(llvm::StringRef what, llvm::StringRef name) {
  return (llvm::Twine("cannot infer the type of ") + what +
          " from 'undefined'; declare the type of the object, e.g. "
          "'const o: { " +
          name + "?: int } = ...'")
      .str();
}

std::string ErrorTextObjectLiteral::missing(llvm::StringRef name, llvm::StringRef target) {
  return (llvm::Twine("field '") + name + "' is missing; type '" + target + "' requires it").str();
}

std::string ErrorTextObjectLiteral::overloads(llvm::StringRef target, llvm::StringRef name) {
  return (llvm::Twine("an object literal cannot be of type '") + target + "': its method '" + name +
          "' has several signatures, and only a class implements "
          "those, each with a variant of its own")
      .str();
}

std::string ErrorTextSpreadLiteral::undefined() {
  return "the object may be undefined; check it before spreading it";
}

std::string ErrorTextSpreadLiteral::notObject(llvm::StringRef type, bool indices) {
  std::string message =
      (llvm::Twine("only objects can be spread into an object literal, found '") + type + "'")
          .str();
  if (indices) {
    message += "; JavaScript would make its indices the keys";
  }

  return message;
}

std::string ErrorTextSpreadLiteral::methods() {
  return "an object with methods cannot be spread into an object "
         "literal yet; copy the fields by name";
}

std::string ErrorTextSpreadLiteral::twoTypes(llvm::StringRef name, llvm::StringRef before,
                                             llvm::StringRef type, llvm::StringRef spread) {
  return (llvm::Twine("field '") + name + "' is '" + before + "' before the spread and '" + type +
          "' in '" + spread + "'; a field has one type")
      .str();
}

std::string ErrorTextObjectLiteral::keyedName(llvm::StringRef type) {
  return (llvm::Twine("the field that '") + type + "' names").str();
}

std::string ErrorTextObjectLiteral::fieldName(llvm::StringRef name) {
  return (llvm::Twine("field '") + name + "'").str();
}

}
