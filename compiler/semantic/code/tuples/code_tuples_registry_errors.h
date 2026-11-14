#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeTuplesRegistryCheckTupleJoinErrorCode {
  ALREADY_REPORTED,
  WRONG_ARITY,
  ELEMENT_NOT_TEXT,
};

struct CodeTuplesRegistryCheckTupleJoinError {
  CodeTuplesRegistryCheckTupleJoinErrorCode code;
  std::string message;
};

enum class CodeTuplesRegistryCheckTupleMemberErrorCode {
  NO_SUCH_PROPERTY,
};

struct CodeTuplesRegistryCheckTupleMemberError {
  CodeTuplesRegistryCheckTupleMemberErrorCode code;
  std::string message;
};

enum class CodeTuplesRegistryCheckTupleMethodErrorCode {
  ALREADY_REPORTED,
  JOIN_FAILED,
  FIXED_LENGTH,
  COPY_FIRST,
  DESTRUCTURE_INSTEAD,
};

struct CodeTuplesRegistryCheckTupleMethodError {
  CodeTuplesRegistryCheckTupleMethodErrorCode code;
  std::string message;
};

class ErrorTextTupleJoin {
public:
  static std::string notText(size_t index, llvm::StringRef tuple, llvm::StringRef element);
};

class ErrorTextTupleMember {
public:
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextTupleMethod {
public:
  static std::string fixedLen(llvm::StringRef name, llvm::StringRef text);
  static std::string copyFirst(llvm::StringRef name, llvm::StringRef text);
  static std::string destruct(llvm::StringRef name, llvm::StringRef text);
};

}
