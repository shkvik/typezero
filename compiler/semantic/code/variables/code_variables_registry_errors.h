#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeVariablesRegistryInitHasTypeErrorCode {
  VOID_INITIALIZER,
  UNDEFINED_INITIALIZER,
};

struct CodeVariablesRegistryInitHasTypeError {
  CodeVariablesRegistryInitHasTypeErrorCode code;
  std::string message;
};

enum class CodeVariablesRegistryConstValueErrorCode {
  CONST_WITHOUT_VALUE,
};

struct CodeVariablesRegistryConstValueError {
  CodeVariablesRegistryConstValueErrorCode code;
  std::string message;
};

enum class CodeVariablesRegistryDestructurableErrorCode {
  NOT_AN_ARRAY,
  ARRAY_MAY_BE_UNDEFINED,
};

struct CodeVariablesRegistryDestructurableError {
  CodeVariablesRegistryDestructurableErrorCode code;
  std::string message;
};

enum class CodeVariablesRegistryResultCheckedErrorCode {
  PROMISE_NEVER_AWAITED,
  RESULT_NEVER_CHECKED,
};

struct CodeVariablesRegistryResultCheckedError {
  CodeVariablesRegistryResultCheckedErrorCode code;
  std::string message;
};

enum class CodeVariablesRegistryVarTypedErrorCode {
  UNDEFINED_ONLY,
  NEEDS_TYPE,
};

struct CodeVariablesRegistryVarTypedError {
  CodeVariablesRegistryVarTypedErrorCode code;
  std::string message;
};

class ErrorTextVarInit {
public:
  static std::string isVoid();
  static std::string undefined(bool isConst, llvm::StringRef name);
};

class ErrorTextConstValue {
public:
  static std::string missing();
};

class ErrorTextVarShape {
public:
  static std::string notArray(llvm::StringRef type);
  static std::string mayBeUndef(llvm::StringRef type);
};

class ErrorTextResultUse {
public:
  static std::string promise(llvm::StringRef name);
  static std::string unchecked(llvm::StringRef name);
};

class ErrorTextVarTyped {
public:
  static std::string writeType(llvm::StringRef name, llvm::StringRef type);
  static std::string addOne(llvm::StringRef name);
  static std::string example(llvm::StringRef name);
  static std::string undefined(llvm::StringRef hint);
  static std::string needsType(llvm::StringRef name, llvm::StringRef hint);
};

}
