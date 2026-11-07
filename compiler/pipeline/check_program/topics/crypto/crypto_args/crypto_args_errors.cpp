#include "compiler/pipeline/check_program/topics/crypto/crypto_args/crypto_args_errors.h"

namespace tz {

std::string ErrorTextCryptoData::notData(llvm::StringRef what, bool strings, bool buffers,
                                         bool keys, const Type *type) {
  std::string kinds = strings ? "a string or bytes" : "bytes";

  return "the " + what.str() + " is " + kinds + " (a Buffer, a typed array or a DataView" +
         (buffers ? ", an ArrayBuffer" : "") + (keys ? ", a KeyObject" : "") + "); found '" +
         type->name().str() + "'" + (type->isNumeric() ? "; make text of it with 'String(x)'" : "");
}

std::string ErrorTextCryptoData::encoding(llvm::StringRef fn) {
  return "the encoding of '" + fn.str() +
         "' is written as a literal, e.g. 'hex': "
         "the type of what it gives depends on it";
}

std::string ErrorTextCryptoOptions::literal(llvm::StringRef fn, llvm::StringRef first) {
  return "the options of '" + fn.str() +
         "' are written in the call, as an object literal, e.g. '{ " + first.str() + ": ... }'";
}

std::string ErrorTextCryptoOptions::byName(llvm::StringRef fn, llvm::StringRef first) {
  return "write each option of '" + fn.str() + "' by its name, e.g. '{ " + first.str() + ": ... }'";
}

std::string ErrorTextCryptoOptions::unknown(llvm::StringRef fn, llvm::StringRef name,
                                            llvm::ArrayRef<llvm::StringRef> allowed) {
  std::string list;
  for (size_t i = 0; i < allowed.size(); ++i) {
    list += (i == 0 ? "" : i + 1 == allowed.size() ? " and " : ", ") + allowed[i].str();
  }

  return "'" + fn.str() + "' has no option '" + name.str() + "'; it takes " + list;
}

std::string ErrorTextCryptoOptions::needsKey(llvm::StringRef fn) {
  return "the options of '" + fn.str() + "' need 'key'";
}

std::string ErrorTextCryptoChoice::list(llvm::ArrayRef<llvm::StringRef> values) {
  std::string list;
  for (size_t i = 0; i < values.size(); ++i) {
    list += (i == 0 ? "'" : i + 1 == values.size() ? "' or '" : "', '") + values[i].str();
  }

  return list + "'";
}

std::string ErrorTextCryptoChoice::literal(llvm::StringRef what,
                                           llvm::ArrayRef<llvm::StringRef> values) {
  return "the " + what.str() + " is written as a literal: " + ErrorTextCryptoChoice::list(values);
}

std::string ErrorTextCryptoChoice::unknown(llvm::StringRef what, llvm::StringRef text,
                                           llvm::ArrayRef<llvm::StringRef> values) {
  return "unknown " + what.str() + " '" + text.str() + "'; it is " +
         ErrorTextCryptoChoice::list(values);
}

std::string ErrorTextCryptoHash::anyLength(llvm::StringRef text) {
  return "'" + text.str() +
         "' gives output of any length, which HMAC and the "
         "KDFs cannot use; use a hash such as 'sha256'";
}

std::string ErrorTextCryptoHash::turnedOff(llvm::StringRef text) {
  return "'" + text.str() +
         "' is turned off in node 24 (it throws "
         "ERR_OSSL_EVP_UNSUPPORTED) and in tz; use 'sha256'";
}

std::string ErrorTextCryptoHash::unknown(llvm::StringRef text) {
  return "unknown hash '" + text.str() +
         "'; node has those getHashes() lists, e.g. "
         "'sha256', 'sha512', 'sha3-256', 'md5'";
}

std::string ErrorTextCryptoCipher::turnedOff(llvm::StringRef text) {
  return "'" + text.str() +
         "' is turned off in node 24 (it throws "
         "ERR_OSSL_EVP_UNSUPPORTED) and in tz; use "
         "'aes-256-gcm'";
}

std::string ErrorTextCryptoCipher::unknown(llvm::StringRef text) {
  return "unknown cipher '" + text.str() +
         "'; node has those getCiphers() lists, "
         "e.g. 'aes-256-gcm', 'aes-256-cbc', "
         "'chacha20-poly1305'";
}

std::string ErrorTextCryptoCipher::keyBytes(llvm::StringRef name, int64_t want, int64_t have) {
  return name.str() + " needs a " + std::to_string(want) + "-byte key, but this one has " +
         std::to_string(have) + " bytes";
}

std::string ErrorTextCryptoCipher::emptyIv(llvm::StringRef name) {
  return name.str() + " needs an IV of 1 byte or more, but it is empty";
}

std::string ErrorTextCryptoCipher::ivBytes(llvm::StringRef name, int64_t want, int64_t have) {
  return name.str() + " needs a " + std::to_string(want) + "-byte IV, but this one has " +
         std::to_string(have) + " bytes";
}

std::string ErrorTextCryptoNumber::whole(llvm::StringRef name, llvm::StringRef least,
                                         llvm::StringRef value) {
  return name.str() + " must be a whole number of " + least.str() + " or more, not " + value.str();
}

std::string ErrorTextCryptoKey::keyObject(llvm::StringRef what, llvm::StringRef type) {
  return "the " + what.str() + " is a KeyObject; found '" + type.str() + "'";
}

std::string ErrorTextCryptoKey::callback(llvm::StringRef fn, llvm::StringRef hint) {
  return "'" + fn.str() +
         "' with a callback is not supported: tz has no callback forms "
         "of node:crypto (docs/crypto.md, section 5); call it without one "
         "and use what it returns: " +
         hint.str() + ", or run it on the thread pool with '" + fn.str() +
         "Async' of \"tz:crypto\": 'await " + fn.str() + "Async(...)'";
}

std::string ErrorTextCryptoLabel::key() {
  return "key";
}

std::string ErrorTextCryptoLabel::signature() {
  return "encoding of the signature";
}

}
