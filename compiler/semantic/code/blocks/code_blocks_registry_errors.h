#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeBlocksRegistryCheckFunctionStmtErrorCode {
  NOT_IN_BLOCK,
};

struct CodeBlocksRegistryCheckFunctionStmtError {
  CodeBlocksRegistryCheckFunctionStmtErrorCode code;
  std::string message;
};

enum class CodeBlocksRegistryCheckBodyErrorCode {
  DECLARATION_AS_BODY,
};

struct CodeBlocksRegistryCheckBodyError {
  CodeBlocksRegistryCheckBodyErrorCode code;
  std::string message;
};

class ErrorTextFunctionStmt {
public:
  static std::string nested();
};

class ErrorTextBody {
public:
  static std::string declared();
};

}
