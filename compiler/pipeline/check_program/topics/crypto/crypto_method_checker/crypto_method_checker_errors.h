#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CryptoMethodCheckerIssuerErrorCode {
  NOT_A_CERTIFICATE,
};

struct CryptoMethodCheckerIssuerError {
  CryptoMethodCheckerIssuerErrorCode code;
  std::string message;
};

class ErrorTextCryptoMethod {
public:
  static std::string issuer(llvm::StringRef type);
  static std::string stream(llvm::StringRef name, llvm::StringRef type, Type::Crypto kind,
                            bool cipher);
  static std::string noMethod(llvm::StringRef name, llvm::StringRef type, llvm::StringRef methods);
};

}
