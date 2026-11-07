#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextCryptoValue {
public:
  static std::string constants();
  static std::string isType(llvm::StringRef name);
  static std::string constant(llvm::StringRef name);
};

class ErrorTextCryptoMember {
public:
  static std::string keyDetails();
  static std::string issuerCert();
  static std::string onlyCalled(llvm::StringRef name, llvm::StringRef type,
                                llvm::StringRef methods);
};

class ErrorTextCryptoMake {
public:
  static std::string notNode(llvm::StringRef cls);
  static std::string x509();
};

class ErrorTextX509 {
public:
  static std::string noStatic(llvm::StringRef name);
  static std::string parseArgs();
  static std::string data();
};

class ErrorTextGlobalCrypto {
public:
  static std::string subtleCall();
  static std::string noMember(llvm::StringRef name);
  static std::string onlyCalled(llvm::StringRef name);
};

}
