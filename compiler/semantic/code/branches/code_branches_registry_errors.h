#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeBranchesRegistryValueTypeErrorCode {
  VOID_VALUE,
};

struct CodeBranchesRegistryValueTypeError {
  CodeBranchesRegistryValueTypeErrorCode code;
  std::string message;
};

enum class CodeBranchesRegistryCaseTypeErrorCode {
  NOT_COMPARABLE,
  RESULTS_COMPARED,
};

struct CodeBranchesRegistryCaseTypeError {
  CodeBranchesRegistryCaseTypeErrorCode code;
  std::string message;
};

enum class CodeBranchesRegistryCasesHandledErrorCode {
  CASES_LEFT,
};

struct CodeBranchesRegistryCasesHandledError {
  CodeBranchesRegistryCasesHandledErrorCode code;
  std::string message;
};

class ErrorTextValueType {
public:
  static std::string isVoid();
};

class ErrorTextCaseType {
public:
  static std::string mismatch(llvm::StringRef test, llvm::StringRef value);
  static std::string results(llvm::StringRef test, llvm::StringRef value);
};

class ErrorTextCasesLeft {
public:
  static std::string unhandled(llvm::StringRef names, llvm::StringRef first);
};

}
