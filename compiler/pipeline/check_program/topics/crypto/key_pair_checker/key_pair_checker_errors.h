#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class KeyPairCheckerKeyOptionsErrorCode {
  NO_MODULUS_LENGTH,
  NO_NAMED_CURVE,
};

struct KeyPairCheckerKeyOptionsError {
  KeyPairCheckerKeyOptionsErrorCode code;
  std::string message;
};

class ErrorTextKeyPair {
public:
  static std::string option(llvm::StringRef name);
  static std::string modulus(llvm::StringRef type);
  static std::string curve();
  static std::string small(llvm::StringRef bits);
  static std::string noFormat(llvm::StringRef option);
  static std::string noType(llvm::StringRef option, bool publicKey);
  static std::string passphrase();
};

class ErrorTextKeyPairLabel {
public:
  static std::string pairType();
  static std::string format();
  static std::string keyType();
};

}
