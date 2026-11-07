#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ModuleDeclarerFindExportErrorCode {
  NOT_EXPORTED,
  NOT_EXPORTED_BY_LIBRARY,
  DECLARED_WITHOUT_EXPORT,
};

struct ModuleDeclarerFindExportError {
  ModuleDeclarerFindExportErrorCode code;
  std::string message;
};

class ErrorTextFindExport {
public:
  static std::string missing(llvm::StringRef path, llvm::StringRef name);
  static std::string library(llvm::StringRef path, llvm::StringRef name, llvm::StringRef hint);
  static std::string unmarked(llvm::StringRef path, llvm::StringRef name, llvm::StringRef file);
};

enum class ModuleDeclarerExportSourceErrorCode {
  NEVERTHROW,
  BUILTIN_MODULE,
};

struct ModuleDeclarerExportSourceError {
  ModuleDeclarerExportSourceErrorCode code;
  std::string message;
};

class ErrorTextExportSource {
public:
  static std::string neverthrow();
  static std::string builtin(llvm::StringRef path);
};

enum class ModuleDeclarerLocalExportErrorCode {
  FROM_BUILTIN_MODULE,
  NOT_FOUND,
  ALREADY_REPORTED,
};

struct ModuleDeclarerLocalExportError {
  ModuleDeclarerLocalExportErrorCode code;
  std::string message;
};

class ErrorTextLocalExport {
public:
  static std::string builtin(llvm::StringRef name);
  static std::string notFound(llvm::StringRef name);
};

enum class ModuleDeclarerFreeExportErrorCode {
  DUPLICATE_EXPORT,
};

struct ModuleDeclarerFreeExportError {
  ModuleDeclarerFreeExportErrorCode code;
  std::string message;
};

class ErrorTextFreeExport {
public:
  static std::string previous();
  static std::string duplicate(llvm::StringRef alias);
};

enum class ModuleDeclarerStarExportErrorCode {
  EXPORTED_BY_TWO,
};

struct ModuleDeclarerStarExportError {
  ModuleDeclarerStarExportErrorCode code;
  std::string message;
};

class ErrorTextStarExport {
public:
  static std::string twice(llvm::StringRef name, llvm::StringRef first, llvm::StringRef second);
};

}
