#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

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
