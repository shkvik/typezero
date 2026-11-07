#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class UtilityTypesResolveUtilityTypeErrorCode {
  NOT_A_UTILITY,
  WRONG_ARGUMENT_COUNT,
  NOT_A_FUNCTION_TYPE,
  NOT_A_STRING_TYPE,
};

struct UtilityTypesResolveUtilityTypeError {
  UtilityTypesResolveUtilityTypeErrorCode code;
  std::string message;
};

class ErrorTextUtility {
public:
  static std::string count(llvm::StringRef name, int count);
  static std::string noFunction(llvm::StringRef name, llvm::StringRef type);
  static std::string notString(llvm::StringRef name, llvm::StringRef type);
};

enum class UtilityTypesCheckGenericIndexErrorCode {
  NOT_A_GENERIC_INDEX,
  NOT_INDEXED_BY_KEY,
};

struct UtilityTypesCheckGenericIndexError {
  UtilityTypesCheckGenericIndexErrorCode code;
  std::string message;
};

class ErrorTextGenericIndex {
public:
  static std::string notByKey(llvm::StringRef object, llvm::StringRef key);
};

}
