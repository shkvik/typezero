#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class WebCryptoCheckerSubtleShapeErrorCode {
  NOT_AN_INTRINSIC,
};

struct WebCryptoCheckerSubtleShapeError {
  WebCryptoCheckerSubtleShapeErrorCode code;
  std::string message;
};

enum class WebCryptoCheckerLiteralAlgorithmErrorCode {
  NOT_A_LITERAL,
};

struct WebCryptoCheckerLiteralAlgorithmError {
  WebCryptoCheckerLiteralAlgorithmErrorCode code;
  std::string message;
};

enum class WebCryptoCheckerSubtleAvailableErrorCode {
  NO_SUBTLE,
};

struct WebCryptoCheckerSubtleAvailableError {
  WebCryptoCheckerSubtleAvailableErrorCode code;
  std::string message;
};

class ErrorTextSubtle {
public:
  static std::string literal(llvm::StringRef name);
  static std::string noSubtle();
};

}
