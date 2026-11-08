#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class VarCheckerInitHasTypeErrorCode {
  VOID_INITIALIZER,
  UNDEFINED_INITIALIZER,
};

struct VarCheckerInitHasTypeError {
  VarCheckerInitHasTypeErrorCode code;
  std::string message;
};

class ErrorTextVarInit {
public:
  static std::string isVoid();
  static std::string undefined(bool isConst, llvm::StringRef name);
};

enum class VarCheckerConstValueErrorCode {
  CONST_WITHOUT_VALUE,
};

struct VarCheckerConstValueError {
  VarCheckerConstValueErrorCode code;
  std::string message;
};

class ErrorTextConstValue {
public:
  static std::string missing();
};

enum class VarCheckerDestructurableErrorCode {
  NOT_AN_ARRAY,
  ARRAY_MAY_BE_UNDEFINED,
};

struct VarCheckerDestructurableError {
  VarCheckerDestructurableErrorCode code;
  std::string message;
};

class ErrorTextVarShape {
public:
  static std::string notArray(llvm::StringRef type);
  static std::string mayBeUndef(llvm::StringRef type);
};

enum class VarCheckerResultCheckedErrorCode {
  PROMISE_NEVER_AWAITED,
  RESULT_NEVER_CHECKED,
};

struct VarCheckerResultCheckedError {
  VarCheckerResultCheckedErrorCode code;
  std::string message;
};

class ErrorTextResultUse {
public:
  static std::string promise(llvm::StringRef name);
  static std::string unchecked(llvm::StringRef name);
};

enum class VarCheckerVarTypedErrorCode {
  UNDEFINED_ONLY,
  NEEDS_TYPE,
};

struct VarCheckerVarTypedError {
  VarCheckerVarTypedErrorCode code;
  std::string message;
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
