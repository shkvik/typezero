#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ReflectFunction {
  GET,
  APPLY,
  CONSTRUCT,
  FILL,
  DEFINE_METADATA,
  GET_METADATA,
  GET_OWN_METADATA,
  HAS_METADATA,
  HAS_OWN_METADATA,
  DELETE_METADATA,
  METADATA,
  GET_METADATA_KEYS,
  GET_OWN_METADATA_KEYS,
};

struct ReflectFunctionInfo {
  llvm::StringLiteral name;
  ReflectFunction value;
};

class ReflectFunctionTable {
public:
  static std::optional<ReflectFunction> find(llvm::StringRef name);
  static llvm::StringRef getName(ReflectFunction function);
};

}
