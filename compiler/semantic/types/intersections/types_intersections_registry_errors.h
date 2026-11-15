#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypesIntersectionsRegistryResolveIntersectionErrorCode {
  FIELD_TYPES_DIFFER,
  NOT_AN_OBJECT,
};

struct TypesIntersectionsRegistryResolveIntersectionError {
  TypesIntersectionsRegistryResolveIntersectionErrorCode code;
  std::string message;
};

class ErrorTextIntersection {
public:
  static std::string fieldTypes(llvm::StringRef field, llvm::StringRef one, llvm::StringRef other);
  static std::string notObject(llvm::StringRef type);
};

}
