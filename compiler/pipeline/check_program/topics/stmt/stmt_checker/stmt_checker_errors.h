#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class StmtCheckerCheckFunctionStmtErrorCode {
  NOT_IN_BLOCK,
};

struct StmtCheckerCheckFunctionStmtError {
  StmtCheckerCheckFunctionStmtErrorCode code;
  std::string message;
};

class ErrorTextFunctionStmt {
public:
  static std::string nested();
};

enum class StmtCheckerCheckBodyErrorCode {
  DECLARATION_AS_BODY,
};

struct StmtCheckerCheckBodyError {
  StmtCheckerCheckBodyErrorCode code;
  std::string message;
};

class ErrorTextBody {
public:
  static std::string declared();
};

}
