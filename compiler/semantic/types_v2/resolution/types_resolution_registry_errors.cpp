#include "compiler/semantic/types_v2/resolution/types_resolution_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextTypeUse::voidType() {
  return "'void' can only be used as a return type or a type argument";
}

std::string ErrorTextTypeUse::undefined() {
  return "the type 'undefined' alone is not supported; use 'T | undefined'";
}

std::string ErrorTextTypeName::enumMember(llvm::StringRef owner, llvm::StringRef member) {
  return (llvm::Twine("enum '") + owner + "' has no member '" + member + "'").str();
}

std::string ErrorTextTypeName::notFound(llvm::StringRef name) {
  return (llvm::Twine("cannot find type '") + name + "'").str();
}

std::string ErrorTextTypeName::arrayArg() {
  return "'Array' needs an element type, e.g. 'Array<int>' or 'int[]'";
}

std::string ErrorTextTypeName::resultArgs(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs the types of its value and its error, e.g. '" + name +
          "<int, string>'")
      .str();
}

std::string ErrorTextTypeName::valuesArg(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs the type of the values it gives, e.g. '" + name +
          "<int>'")
      .str();
}

std::string ErrorTextTypeName::promiseArg() {
  return "'Promise' needs the type of its value, e.g. 'Promise<int>' or "
         "'Promise<void>'";
}

std::string ErrorTextTypeName::nullType() {
  return "'null' is not supported: the only value for nothing is 'undefined'; "
         "write 'T | undefined'";
}

std::string ErrorTextTypeName::longType() {
  return "there is no 'long': 'int' is 64 bits";
}

std::string ErrorTextTypeName::shared() {
  return "'SharedArrayBuffer' is not supported yet: it comes with the shared "
         "memory of async (docs/bytes.md, section 7); use 'ArrayBuffer'";
}

std::string ErrorTextTypeName::bigintType() {
  return "the type 'bigint'";
}

std::string ErrorTextTypeName::bigintName(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "'").str();
}

std::string ErrorTextGenericRef::bufferArg(llvm::StringRef name) {
  return (llvm::Twine("the type argument of '") + name +
          "' can only be 'ArrayBuffer'; or leave it out")
      .str();
}

std::string ErrorTextGenericRef::oneArg(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' takes one type argument").str();
}

std::string ErrorTextGenericRef::plainClass(llvm::StringRef name) {
  return (llvm::Twine("class '") + name + "' is not generic; remove the type arguments").str();
}

std::string ErrorTextGenericRef::plainAlias(llvm::StringRef name) {
  return (llvm::Twine("type '") + name + "' is not generic; remove the type arguments").str();
}

std::string ErrorTextGenericRef::descriptor() {
  return "'MethodDescriptor' takes two type arguments: the types of the "
         "method's parameters and what it returns, e.g. "
         "'MethodDescriptor<[int, int], int>'";
}

std::string ErrorTextGenericRef::paramList() {
  return "the first type argument of 'MethodDescriptor' is the list of "
         "the method's parameter types, a tuple such as '[int, "
         "string]', or 'A' of '<A extends unknown[]>'";
}

std::string ErrorTextGenericRef::instanceOf(llvm::StringRef type) {
  return (llvm::Twine("'InstanceType' takes a class as a value, e.g. "
                      "'InstanceType<typeof Point>', found '") +
          type + "'")
      .str();
}

std::string ErrorTextGenericRef::noSupport(llvm::StringRef name) {
  return (llvm::Twine("generic type '") + name + "' is not supported").str();
}

std::string ErrorTextTypeShape::arrayOf(llvm::StringRef type) {
  return (llvm::Twine("arrays of '") + type + "' are not supported").str();
}

std::string ErrorTextTypeShape::voidUnion() {
  return "'void' cannot be part of a union";
}

std::string ErrorTextTypeShape::nullUnion() {
  return "'null' is not supported: the only value for nothing is 'undefined'; "
         "write 'T | undefined' (null exists only in Json values)";
}

std::string ErrorTextTypeShape::noUnion(llvm::StringRef type, llvm::StringRef problem) {
  return (llvm::Twine("union '") + type + "' is not supported: " + problem).str();
}

std::string ErrorTextTypeShape::predicate(llvm::StringRef name) {
  return (llvm::Twine("a type predicate, '") + name +
          " is T', can only be the return type of a function")
      .str();
}

std::string ErrorTextTypeShape::inferOnly() {
  return "'infer' declares a type only in the 'extends' clause of a conditional "
         "type, e.g. 'T extends Array<infer U> ? U : never'";
}

std::string ErrorTextTypeShape::restArray() {
  return "a rest parameter must be an array, e.g. '...xs: int[]'";
}

std::string ErrorTextTypeShape::paramOrder() {
  return "a required parameter cannot follow an optional one";
}

std::string ErrorTextTypeShape::tupleRest() {
  return "a rest element in a tuple type is not supported: a tuple has a fixed "
         "length; use an array, e.g. 'int[]'";
}

std::string ErrorTextTypeShape::typeList(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is a list of types; spread it into the tuple, '..." + name +
          "'")
      .str();
}

std::string ErrorTextTypeFields::signatures(llvm::StringRef name) {
  return (llvm::Twine("several signatures of method '") + name +
          "' are supported only in an interface; declare one, "
          "'interface Name { ... }'")
      .str();
}

std::string ErrorTextTypeFields::duplicate(llvm::StringRef type, llvm::StringRef name) {
  return (llvm::Twine("duplicate signature '") + type + "' of method '" + name +
          "': the signatures must differ in their parameters")
      .str();
}

std::string ErrorTextTypeFields::sameField(llvm::StringRef name) {
  return (llvm::Twine("duplicate field '") + name + "'").str();
}

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

}
