#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypeOperatorsResolveTypeOpErrorCode {
  NOT_AN_OBJECT_TYPE,
  NO_SUCH_MEMBER,
  TYPE_OF_UNKNOWN,
  NO_SUCH_FIELD,
  RENAME_NOT_STRING,
  KEYS_NOT_STRINGS,
};

struct TypeOperatorsResolveTypeOpError {
  TypeOperatorsResolveTypeOpErrorCode code;
  std::string message;
};

class ErrorTextTypeOp {
public:
  static std::string keyOf(llvm::StringRef type);
  static std::string noMember(llvm::StringRef object, llvm::StringRef key);
  static std::string typeOf(llvm::StringRef name);
  static std::string noField(llvm::StringRef type, llvm::StringRef field);
  static std::string rename(llvm::StringRef name);
  static std::string keys(llvm::StringRef key);
};

}
