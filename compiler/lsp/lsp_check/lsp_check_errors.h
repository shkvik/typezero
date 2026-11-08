#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LspCheckReadRequestErrorCode {
  REQUEST_NOT_READ,
  REQUEST_NOT_JSON,
  NO_ENTRY,
};

struct LspCheckReadRequestError {
  LspCheckReadRequestErrorCode code;
  std::string message;
};

enum class LspCheckRunErrorCode {
  BAD_REQUEST,
  RESPONSE_NOT_WRITTEN,
};

struct LspCheckRunError {
  LspCheckRunErrorCode code;
  std::string message;
};

class ErrorTextLspCheck {
public:
  static std::string unread(llvm::StringRef path, llvm::StringRef why);
  static std::string notJson(llvm::StringRef path, llvm::StringRef why);
  static std::string noEntry(llvm::StringRef path);
  static std::string unwritten(llvm::StringRef path, llvm::StringRef why);
  static std::string packages();
};

}
