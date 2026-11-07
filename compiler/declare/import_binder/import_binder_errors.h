#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ImportBinderWholeImportErrorCode {
  DEFAULT_IMPORT,
  STAR_IMPORT,
};

struct ImportBinderWholeImportError {
  ImportBinderWholeImportErrorCode code;
  std::string message;
};

class ErrorTextWholeImport {
public:
  static std::string byDefault(llvm::StringRef path);
  static std::string star(llvm::StringRef path);
};

enum class ImportBinderFreeNameErrorCode {
  DECLARED_IN_FILE,
  IMPORTED_FROM_NODE,
  IMPORTED_TWICE,
};

struct ImportBinderFreeNameError {
  ImportBinderFreeNameErrorCode code;
  std::string message;
};

class ErrorTextFreeName {
public:
  static std::string previous();
  static std::string duplicate(llvm::StringRef alias);
  static std::string declared(llvm::StringRef alias);
  static std::string conflicts(llvm::StringRef alias, llvm::StringRef name, llvm::StringRef path);
};

}
