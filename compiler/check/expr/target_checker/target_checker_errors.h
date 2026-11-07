#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TargetCheckerIndexNotBytesErrorCode {
  BYTES_INDEXED,
};

struct TargetCheckerIndexNotBytesError {
  TargetCheckerIndexNotBytesErrorCode code;
  std::string message;
};

class ErrorTextIndexTarget {
public:
  static std::string bytes(llvm::StringRef type, bool dataView);
  static std::string undefined();
  static std::string immutable();
  static std::string arrayUndef();
  static std::string notIndexed(llvm::StringRef type);
};

enum class TargetCheckerObjectDefinedErrorCode {
  MAY_BE_UNDEFINED,
};

struct TargetCheckerObjectDefinedError {
  TargetCheckerObjectDefinedErrorCode code;
  std::string message;
};

enum class TargetCheckerIndexWritableErrorCode {
  STRING_IMMUTABLE,
  ARRAY_MAY_BE_UNDEFINED,
  NOT_INDEXABLE,
};

struct TargetCheckerIndexWritableError {
  TargetCheckerIndexWritableErrorCode code;
  std::string message;
};

enum class TargetCheckerDictDefinedErrorCode {
  MAY_BE_UNDEFINED,
};

struct TargetCheckerDictDefinedError {
  TargetCheckerDictDefinedErrorCode code;
  std::string message;
};

class ErrorTextMemberTarget {
public:
  static std::string undefined(llvm::StringRef name);
  static std::string fixedSize(llvm::StringRef type);
  static std::string noAssign(llvm::StringRef name, llvm::StringRef type);
  static std::string onlyAssign();
  static std::string textLength();
};

enum class TargetCheckerBytesMemberErrorCode {
  LENGTH_FIXED,
  NOT_WRITABLE,
};

struct TargetCheckerBytesMemberError {
  TargetCheckerBytesMemberErrorCode code;
  std::string message;
};

enum class TargetCheckerMemberWritableErrorCode {
  COMPOUND_LENGTH,
  STRING_LENGTH,
  NOT_WRITABLE,
};

struct TargetCheckerMemberWritableError {
  TargetCheckerMemberWritableErrorCode code;
  std::string message;
};

class ErrorTextAssignTarget {
public:
  static std::string arrayForm();
  static std::string objectForm();
  static std::string notTarget();
};

enum class TargetCheckerNotThisErrorCode {
  ASSIGN_TO_THIS,
};

struct TargetCheckerNotThisError {
  TargetCheckerNotThisErrorCode code;
  std::string message;
};

class ErrorTextVarTarget {
public:
  static std::string toThis();
  static std::string constant(llvm::StringRef name, bool function);
};

enum class TargetCheckerVariableMutableErrorCode {
  CONSTANT,
};

struct TargetCheckerVariableMutableError {
  TargetCheckerVariableMutableErrorCode code;
  std::string message;
};

}
