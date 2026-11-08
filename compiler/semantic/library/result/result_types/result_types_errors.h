#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ResultTypesImportSourceErrorCode {
  NEVERTHROW_IMPORT,
};

struct ResultTypesImportSourceError {
  ResultTypesImportSourceErrorCode code;
  std::string message;
};

enum class ResultTypesResultTypeArgsErrorCode {
  NOT_TWO_TYPE_ARGUMENTS,
};

struct ResultTypesResultTypeArgsError {
  ResultTypesResultTypeArgsErrorCode code;
  std::string message;
};

class ErrorTextResultType {
public:
  static std::string neverthrow();
  static std::string typeArgs(llvm::StringRef name);
};

}
