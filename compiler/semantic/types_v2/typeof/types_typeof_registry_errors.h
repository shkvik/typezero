#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypesTypeofRegistryResolveTypeOpErrorCode {
  NOT_AN_OBJECT_TYPE,
  NO_SUCH_MEMBER,
  TYPE_OF_UNKNOWN,
  NO_SUCH_FIELD,
  RENAME_NOT_STRING,
  KEYS_NOT_STRINGS,
};

struct TypesTypeofRegistryResolveTypeOpError {
  TypesTypeofRegistryResolveTypeOpErrorCode code;
  std::string message;
};

class ErrorTextTypeOf {
public:
  static std::string typeOf(llvm::StringRef name);
  static std::string noField(llvm::StringRef type, llvm::StringRef field);
};

}
