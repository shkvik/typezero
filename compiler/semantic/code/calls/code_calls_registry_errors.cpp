#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextValueCall::takesThis() {
  return "the function takes its 'this' first, which a plain call "
         "does not pass; call it with '.call(obj, ...)' or "
         "'.apply(obj, args)'";
}

std::string ErrorTextCallTypeArgs::forGeneric() {
  return "type arguments can be given only to a generic function";
}

std::string ErrorTextCallTypeArgs::notGeneric(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is not a generic function; remove the type arguments").str();
}

std::string ErrorTextCallee::undefined() {
  return "the function may be undefined; check it first, or call it with "
         "'?.()', which gives undefined then";
}

std::string ErrorTextCallee::notFunc(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' is not a function; it has type '" + type + "'").str();
}

std::string ErrorTextCallee::uncallable(llvm::StringRef type) {
  return (llvm::Twine("a value of type '") + type + "' cannot be called").str();
}

std::string ErrorTextSpread::notRest() {
  return "a spread argument can only be passed to a rest parameter, "
         "Math.min, Math.max or push";
}

std::string ErrorTextCallMethod::undefined(llvm::StringRef name) {
  return (llvm::Twine("the value may be undefined; check it before calling '") + name + "'").str();
}

std::string ErrorTextCallMethod::setsThis(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is not supported: it sets 'this', and a "
          "method taken as a value is bound to its "
          "object already; call the function "
          "directly")
      .str();
}

std::string ErrorTextCallMethod::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextCallName::noSupport(llvm::StringRef name, llvm::StringRef why) {
  return (llvm::Twine("'") + name + "' is not supported: " + why).str();
}

std::string ErrorTextCallName::notFunc(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is not a function").str();
}

std::string ErrorTextArity::count(unsigned min, unsigned max, size_t got) {
  auto expected = ErrorTextArity::wanted(min, max);

  return (llvm::Twine("expected ") + expected + ", but got " + llvm::Twine(got)).str();
}

std::string ErrorTextArity::tagged(unsigned min, unsigned max, size_t got) {
  auto expected = ErrorTextArity::wanted(min, max);
  auto values = std::string();
  if (got == 2) {
    values = " and 1 value";
  } else if (got != 1) {
    values = " and " + std::to_string(got - 1) + " values";
  }

  return (llvm::Twine("expected ") + expected + ", but the tagged template passes " +
          llvm::Twine(got) + ": the array of its texts" + values)
      .str();
}

std::string ErrorTextArity::wanted(unsigned min, unsigned max) {
  std::string text = std::to_string(min);
  if (max != min) {
    text += max == ~0u ? " or more" : "-" + std::to_string(max);
  }

  return text + (max == 1 && min == 1 ? " argument" : " arguments");
}

std::string ErrorTextSpreadRest::otherType(llvm::StringRef from, llvm::StringRef to) {
  return (llvm::Twine("cannot spread '") + from + "' into '" + to + "'").str();
}

std::string ErrorTextTupleRest::count(size_t expected, size_t got) {
  return (llvm::Twine("expected ") + llvm::Twine(expected) + " arguments, but got " +
          llvm::Twine(got))
      .str();
}

std::string ErrorTextNamespaceNew::namespaced(llvm::StringRef name, llvm::StringRef member) {
  return (llvm::Twine("'") + name + "." + member +
          "' is not supported; of namespaces, only the classes of 'Intl' "
          "can be made with 'new'")
      .str();
}

std::string ErrorTextCryptoNew::cryptoKey() {
  return "a CryptoKey is made by crypto.subtle: "
         "importKey, generateKey, deriveKey or "
         "unwrapKey";
}

std::string ErrorTextCryptoNew::subtle() {
  return "there is one SubtleCrypto: crypto.subtle";
}

std::string ErrorTextSupportedNew::unsupported(llvm::StringRef name, llvm::StringRef why) {
  return (llvm::Twine("'new ") + name + "' is not supported: " + why).str();
}

std::string ErrorTextNoTypeArgs::given(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' takes no type arguments").str();
}

std::string ErrorTextClassFound::notFound(llvm::StringRef name) {
  return (llvm::Twine("cannot find class '") + name + "'").str();
}

std::string ErrorTextNewArray::oneArg() {
  return "'Array' takes one type argument";
}

std::string ErrorTextNewArray::needs() {
  return "'new Array' needs an element type, e.g. 'new Array<int>(n)'";
}

std::string ErrorTextNewArray::length() {
  return "an array length";
}

std::string ErrorTextArrayArgs::literal() {
  return "to create an array from values, use a literal: [a, b, c]";
}

std::string ErrorTextZeroValue::none(llvm::StringRef element) {
  return (llvm::Twine("'new Array<T>(n)' fills the array with a default value, and '") + element +
          "' has none; start from '[]' and use push()")
      .str();
}

std::string ErrorTextUnsupported::why(llvm::StringRef name) {
  if (name == "eval") {
    return "a compiled program cannot run code from a string";
  }
  if (name == "Function") {
    return "a compiled program cannot run code from a string; write the function itself, e.g. "
           "'(a: int) => a'";
  }
  if (name == "Proxy") {
    return "the fields of an object are read and written straight in memory, so nothing can "
           "intercept that";
  }
  if (name == "require") {
    return "files are imported by name: 'import { a } from \"./x\"'";
  }
  if (name == "module" || name == "exports") {
    return "files export by name: 'export function f() {...}', or 'export { a, b }'";
  }
  if (name == "escape") {
    return "it is a legacy function whose '%uXXXX' escapes nothing decodes but 'unescape'; "
           "encode with 'encodeURIComponent(s)'";
  }
  if (name == "unescape") {
    return "it is a legacy function that decodes the '%uXXXX' escapes of 'escape'; decode with "
           "'decodeURIComponent(s)'";
  }

  return "";
}

}
