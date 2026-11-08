#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace tz {

enum class CryptoArgsKeyObjectTypeErrorCode {
  NOT_A_KEY_OBJECT,
};

struct CryptoArgsKeyObjectTypeError {
  CryptoArgsKeyObjectTypeErrorCode code;
  std::string message;
};

class ErrorTextCryptoData {
public:
  static std::string notData(llvm::StringRef what, bool strings, bool buffers, bool keys,
                             const Type *type);
  static std::string encoding(llvm::StringRef fn);
};

class ErrorTextCryptoOptions {
public:
  static std::string literal(llvm::StringRef fn, llvm::StringRef first);
  static std::string byName(llvm::StringRef fn, llvm::StringRef first);
  static std::string unknown(llvm::StringRef fn, llvm::StringRef name,
                             llvm::ArrayRef<llvm::StringRef> allowed);
  static std::string needsKey(llvm::StringRef fn);
};

class ErrorTextCryptoChoice {
public:
  static std::string list(llvm::ArrayRef<llvm::StringRef> values);
  static std::string literal(llvm::StringRef what, llvm::ArrayRef<llvm::StringRef> values);
  static std::string unknown(llvm::StringRef what, llvm::StringRef text,
                             llvm::ArrayRef<llvm::StringRef> values);
};

class ErrorTextCryptoHash {
public:
  static std::string anyLength(llvm::StringRef text);
  static std::string turnedOff(llvm::StringRef text);
  static std::string unknown(llvm::StringRef text);
};

class ErrorTextCryptoCipher {
public:
  static std::string turnedOff(llvm::StringRef text);
  static std::string unknown(llvm::StringRef text);
  static std::string keyBytes(llvm::StringRef name, int64_t want, int64_t have);
  static std::string emptyIv(llvm::StringRef name);
  static std::string ivBytes(llvm::StringRef name, int64_t want, int64_t have);
};

class ErrorTextCryptoNumber {
public:
  static std::string whole(llvm::StringRef name, llvm::StringRef least, llvm::StringRef value);
};

class ErrorTextCryptoKey {
public:
  static std::string keyObject(llvm::StringRef what, llvm::StringRef type);
  static std::string callback(llvm::StringRef fn, llvm::StringRef hint);
};

class ErrorTextCryptoLabel {
public:
  static std::string key();
  static std::string signature();
};

}
