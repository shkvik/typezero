#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ModuleExportFoundErrorCode {
  NO_EXPORT,
  NOT_YET,
  NOT_POSSIBLE,
  ELSEWHERE,
  CALLBACK_FORM,
};

struct ModuleExportFoundError {
  ModuleExportFoundErrorCode code;
  std::string message;
};

class ErrorTextModuleExport {
public:
  static std::string none(llvm::StringRef path, llvm::StringRef name);
  static std::string notYet(llvm::StringRef name, llvm::StringRef path);
};

}
