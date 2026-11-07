#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class BuiltinModuleCheckerKnownModuleErrorCode {
  NO_MODULE,
  NOT_YET,
};

struct BuiltinModuleCheckerKnownModuleError {
  BuiltinModuleCheckerKnownModuleErrorCode code;
  std::string message;
};

enum class BuiltinModuleCheckerExportFoundErrorCode {
  NOT_EXPORTED,
};

struct BuiltinModuleCheckerExportFoundError {
  BuiltinModuleCheckerExportFoundErrorCode code;
  std::string message;
};

class ErrorTextBuiltinModule {
public:
  static std::string none(llvm::StringRef path);
  static std::string notYet(llvm::StringRef path);
};

class ErrorTextNodeImport {
public:
  static std::string conflict(llvm::StringRef alias, llvm::StringRef name, llvm::StringRef path);
  static std::string declared(llvm::StringRef alias);
  static std::string duplicate(llvm::StringRef alias);
  static std::string list(llvm::ArrayRef<llvm::StringRef> used);
  static std::string form(bool isDefault);
  static std::string process(llvm::StringRef form);
  static std::string buffer(llvm::StringRef form, llvm::StringRef path);
  static std::string uses(llvm::StringRef form, llvm::StringRef names, llvm::StringRef path);
  static std::string typical(llvm::StringRef form, llvm::StringRef name, llvm::StringRef path);
};

class ErrorTextNodeValue {
public:
  static std::string notValue(llvm::StringRef label, bool path, llvm::StringRef typical);
  static std::string onlyCalled(llvm::StringRef name, llvm::StringRef label);
  static std::string notFunc(llvm::StringRef name, llvm::StringRef label);
};

}
