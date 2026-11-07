#include "compiler/pipeline/check_program/topics/objects/object_checker/object_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextObjectCall::noSupport(llvm::StringRef fn) {
  return (llvm::Twine("'Object.") + fn +
          "' is not supported; only Object.keys, Object.values, "
          "Object.entries, Object.fromEntries and Object.groupBy are")
      .str();
}

std::string ErrorTextObjectCall::undefined() {
  return "the object may be undefined; check it first";
}

std::string ErrorTextObjectCall::needObject(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'Object.") + fn + "' needs an object, found '" + type + "'").str();
}

std::string ErrorTextObjectCall::emitter(llvm::StringRef fn) {
  return (llvm::Twine("'Object.") + fn +
          "' of an emitter is not supported: its fields are the "
          "listeners it keeps; read the fields of its class by name")
      .str();
}

std::string ErrorTextObjectCall::methods(llvm::StringRef fn) {
  return (llvm::Twine("'Object.") + fn +
          "' of an object with methods is not supported; "
          "read the fields by name")
      .str();
}

std::string ErrorTextObjectCall::noUnion(llvm::StringRef fn, llvm::StringRef type,
                                         llvm::StringRef problem) {
  return (llvm::Twine("'Object.") + fn +
          "' puts the values of all the fields in one array, of the "
          "union of their types, and the fields of '" +
          type + "' have types that no union holds: " + problem + "; read the fields by name")
      .str();
}

std::string ErrorTextObjectCall::noFields(llvm::StringRef type, llvm::StringRef fn) {
  return (llvm::Twine("'") + type + "' has no fields, so 'Object." + fn + "' would always be empty")
      .str();
}

std::string ErrorTextObjectCall::numberEnum(llvm::StringRef fn, llvm::StringRef name,
                                            llvm::StringRef member) {
  return (llvm::Twine("'Object.") + fn + "' of numeric enum '" + name +
          "' is not supported: in JavaScript it mixes the names of the "
          "members with their numbers; list the members you need, e.g. '[" +
          name + "." + member + "]'")
      .str();
}

std::string ErrorTextForIn::indices(llvm::StringRef what, llvm::StringRef text, bool array) {
  std::string forEach =
      array ? (llvm::Twine(" or '") + text + ".forEach((x, i) => ...)'").str() : std::string();

  return (llvm::Twine("'for...in' over ") + what +
          " gives its indices as strings ('0', '1'...), so 'i + 1' would be '01'; walk "
          "the elements with 'for (const x of " +
          text + ")', or the indices with 'for (let i = 0; i < " + text + ".length; i++)'" +
          forEach)
      .str();
}

std::string ErrorTextForIn::undefined() {
  return "the object may be undefined; check it before walking its keys";
}

std::string ErrorTextForIn::dict(llvm::StringRef text, bool isMap) {
  return (llvm::Twine("'for...in' walks the properties of an object, and a ") +
          (isMap ? "Map" : "Set") + " has none in JavaScript; walk its " +
          (isMap ? "keys" : "values") + " with 'for (const x of " + text +
          (isMap ? ".keys()" : "") + ")'")
      .str();
}

std::string ErrorTextForIn::notObject(llvm::StringRef type) {
  return (llvm::Twine("'for...in' walks the keys of an object, found '") + type + "'").str();
}

std::string ErrorTextObjectField::noField(llvm::StringRef name, llvm::StringRef type,
                                          bool inherited) {
  std::string message =
      (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
  if (inherited) {
    message += "; objects inherit nothing from Object.prototype";
  }

  return message;
}

std::string ErrorTextObjectField::maybeUndef() {
  return "the function may be undefined; check it first, or call it with '?.()', "
         "which gives undefined then";
}

std::string ErrorTextObjectField::notFunc(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("field '") + name + "' is not a function; it has type '" + type + "'").str();
}

std::string ErrorTextObjectField::setUndef(llvm::StringRef name) {
  return (llvm::Twine("the value may be undefined; check it before assigning "
                      "to '") +
          name + "'")
      .str();
}

std::string ErrorTextObjectField::tupleLen() {
  return "a tuple has a fixed length, which cannot be changed";
}

std::string ErrorTextObjectField::overloaded(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' is a method of '" + type +
          "' with several signatures and cannot be replaced")
      .str();
}

std::string ErrorTextObjectField::readOnly(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("cannot assign to '") + name + "' because it is a read-only field of '" +
          type + "'")
      .str();
}

std::string ErrorTextForIn::anArray() {
  return "an array";
}

std::string ErrorTextForIn::aString() {
  return "a string";
}

std::string ErrorTextForIn::aTuple() {
  return "a tuple";
}

std::string ErrorTextMismatch::extraField(llvm::StringRef name) {
  return (llvm::Twine(
              "; an object must have exactly the fields of its type, and this one has the extra "
              "field '") +
          name + "'")
      .str();
}

std::string ErrorTextMismatch::noField(llvm::StringRef name) {
  return (llvm::Twine("; it has no field '") + name + "'").str();
}

std::string ErrorTextMismatch::fieldType(llvm::StringRef name, llvm::StringRef here,
                                         llvm::StringRef there, llvm::StringRef to) {
  return (llvm::Twine("; field '") + name + "' is '" + here + "' here and '" + there + "' in '" +
          to + "'")
      .str();
}

std::string ErrorTextMismatch::typeHint(llvm::StringRef to) {
  return (llvm::Twine(": give the object its type where it is created, e.g. 'const p: ") + to +
          " = { ... }'")
      .str();
}

std::string ErrorTextMismatch::readonly(llvm::StringRef name, llvm::StringRef to) {
  return (llvm::Twine("; field '") + name + "' is read-only here, and '" + to +
          "' would let it be changed")
      .str();
}

}
