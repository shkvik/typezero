#include "compiler/check/expr/new_checker/new_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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

std::string ErrorTextArrayArgs::literal() {
  return "to create an array from values, use a literal: [a, b, c]";
}

std::string ErrorTextZeroValue::none(llvm::StringRef element) {
  return (llvm::Twine("'new Array<T>(n)' fills the array with a default value, and '") + element +
          "' has none; start from '[]' and use push()")
      .str();
}

}
