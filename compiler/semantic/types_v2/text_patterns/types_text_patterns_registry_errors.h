#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypesTextPatternsRegistryHoleTypeErrorCode {
  NOT_A_HOLE,
};

struct TypesTextPatternsRegistryHoleTypeError {
  TypesTextPatternsRegistryHoleTypeErrorCode code;
  std::string message;
};

enum class TypesTextPatternsRegistryStringCountErrorCode {
  TOO_MANY_STRINGS,
};

struct TypesTextPatternsRegistryStringCountError {
  TypesTextPatternsRegistryStringCountErrorCode code;
  std::string message;
};

class ErrorTextHoleType {
public:
  static std::string notHole(llvm::StringRef type);
};

class ErrorTextStringCount {
public:
  static std::string tooMany(size_t most);
};

}
