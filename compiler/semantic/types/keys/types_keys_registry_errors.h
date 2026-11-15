#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypesKeysRegistryResolveTypeOpErrorCode {
  NOT_AN_OBJECT_TYPE,
  NO_SUCH_MEMBER,
  TYPE_OF_UNKNOWN,
  NO_SUCH_FIELD,
  RENAME_NOT_STRING,
  KEYS_NOT_STRINGS,
};

struct TypesKeysRegistryResolveTypeOpError {
  TypesKeysRegistryResolveTypeOpErrorCode code;
  std::string message;
};

class ErrorTextKeys {
public:
  static std::string keyOf(llvm::StringRef type);
  static std::string noMember(llvm::StringRef object, llvm::StringRef key);
};

}
