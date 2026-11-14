#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DeclarationNamesRegistryDeclareErrorCode {
  UNDEFINED_AS_NAME,
  DUPLICATE_DECLARATION,
  DUPLICATE_FUNCTION,
};

struct DeclarationNamesRegistryDeclareError {
  DeclarationNamesRegistryDeclareErrorCode code;
  std::string message;
};

class ErrorTextDeclareName {
public:
  static std::string undefined();
  static std::string previous();
  static std::string function(llvm::StringRef name);
  static std::string duplicate(llvm::StringRef name);
};

enum class DeclarationNamesRegistryCheckVisibleErrorCode {
  DECLARED_IN_OTHER_CASE,
};

struct DeclarationNamesRegistryCheckVisibleError {
  DeclarationNamesRegistryCheckVisibleErrorCode code;
  std::string message;
};

class ErrorTextCheckVisible {
public:
  static std::string otherCase(llvm::StringRef name);
};

}
