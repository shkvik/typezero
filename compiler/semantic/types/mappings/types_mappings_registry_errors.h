#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypesMappingsRegistryResolveTypeOpErrorCode {
  NOT_AN_OBJECT_TYPE,
  NO_SUCH_MEMBER,
  TYPE_OF_UNKNOWN,
  NO_SUCH_FIELD,
  RENAME_NOT_STRING,
  KEYS_NOT_STRINGS,
};

struct TypesMappingsRegistryResolveTypeOpError {
  TypesMappingsRegistryResolveTypeOpErrorCode code;
  std::string message;
};

class ErrorTextMappings {
public:
  static std::string rename(llvm::StringRef name);
  static std::string keys(llvm::StringRef key);
};

}
