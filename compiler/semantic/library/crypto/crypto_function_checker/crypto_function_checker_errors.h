#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CryptoFunctionCheckerFilledTargetErrorCode {
  NOT_INTEGERS,
  NOT_BYTES,
};

struct CryptoFunctionCheckerFilledTargetError {
  CryptoFunctionCheckerFilledTargetErrorCode code;
  std::string message;
};

enum class CryptoFunctionCheckerCurveIsKnownErrorCode {
  UNKNOWN_CURVE,
};

struct CryptoFunctionCheckerCurveIsKnownError {
  CryptoFunctionCheckerCurveIsKnownErrorCode code;
  std::string message;
};

class ErrorTextCryptoFunction {
public:
  static std::string callback(llvm::StringRef fn);
  static std::string isClass(llvm::StringRef fn);
  static std::string randomArgs();
  static std::string randomInt(llvm::StringRef type);
  static std::string greater(bool minimum);
  static std::string randomVals(llvm::StringRef type);
  static std::string fillsBytes(llvm::StringRef shown, llvm::StringRef type);
  static std::string fillFits(llvm::StringRef count, llvm::StringRef at);
  static std::string together(llvm::StringRef first, llvm::StringRef second);
  static std::string costN(llvm::StringRef cost);
  static std::string needs(llvm::StringRef shown, llvm::StringRef required);
  static std::string tagLength(llvm::StringRef name);
  static std::string keyCoding();
  static std::string keyLength(llvm::StringRef shown);
  static std::string aesLength();
  static std::string curve(llvm::StringRef curve);
  static std::string dhKeys();
  static std::string bigint();
};

class ErrorTextCryptoFnLabel {
public:
  static std::string offset();
  static std::string size();
  static std::string keyBits();
  static std::string first();
  static std::string second();
  static std::string argon();
  static std::string keyFormat();
  static std::string keyType();
  static std::string privateKey();
  static std::string publicKey();
};

class ErrorTextCryptoPoint {
public:
  static std::string format();
};

}
