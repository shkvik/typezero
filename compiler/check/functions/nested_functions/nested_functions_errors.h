#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class NestedFunctionsUseOrderErrorCode {
  USED_BEFORE_DECLARED,
};

struct NestedFunctionsUseOrderError {
  NestedFunctionsUseOrderErrorCode code;
  std::string message;
};

class ErrorTextUseOrder {
public:
  static std::string early(llvm::StringRef function, llvm::StringRef variable);
  static std::string declared(llvm::StringRef variable);
};

}
