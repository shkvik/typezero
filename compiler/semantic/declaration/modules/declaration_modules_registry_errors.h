#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class DeclarationModulesRegistryFindExportErrorCode {
  NOT_EXPORTED,
  NOT_EXPORTED_BY_LIBRARY,
  DECLARED_WITHOUT_EXPORT,
};

struct DeclarationModulesRegistryFindExportError {
  DeclarationModulesRegistryFindExportErrorCode code;
  std::string message;
};

enum class DeclarationModulesRegistryExportSourceErrorCode {
  NEVERTHROW,
  BUILTIN_MODULE,
};

struct DeclarationModulesRegistryExportSourceError {
  DeclarationModulesRegistryExportSourceErrorCode code;
  std::string message;
};

enum class DeclarationModulesRegistryLocalExportErrorCode {
  FROM_BUILTIN_MODULE,
  NOT_FOUND,
  ALREADY_REPORTED,
};

struct DeclarationModulesRegistryLocalExportError {
  DeclarationModulesRegistryLocalExportErrorCode code;
  std::string message;
};

enum class DeclarationModulesRegistryFreeExportErrorCode {
  DUPLICATE_EXPORT,
};

struct DeclarationModulesRegistryFreeExportError {
  DeclarationModulesRegistryFreeExportErrorCode code;
  std::string message;
};

enum class DeclarationModulesRegistryStarExportErrorCode {
  EXPORTED_BY_TWO,
};

struct DeclarationModulesRegistryStarExportError {
  DeclarationModulesRegistryStarExportErrorCode code;
  std::string message;
};

enum class DeclarationModulesRegistryWholeImportErrorCode {
  DEFAULT_IMPORT,
  STAR_IMPORT,
};

struct DeclarationModulesRegistryWholeImportError {
  DeclarationModulesRegistryWholeImportErrorCode code;
  std::string message;
};

enum class DeclarationModulesRegistryFreeNameErrorCode {
  DECLARED_IN_FILE,
  IMPORTED_FROM_NODE,
  IMPORTED_TWICE,
};

struct DeclarationModulesRegistryFreeNameError {
  DeclarationModulesRegistryFreeNameErrorCode code;
  std::string message;
};

enum class DeclarationModulesRegistryCheckLoadedErrorCode {
  ALREADY_REPORTED,
  READ_WHILE_LOADING,
  NO_TYPE_BEFORE_CALL,
  NO_TYPE_IN_FUNCTION,
};

struct DeclarationModulesRegistryCheckLoadedError {
  DeclarationModulesRegistryCheckLoadedErrorCode code;
  std::string message;
};

class ErrorTextFindExport {
public:
  static std::string missing(llvm::StringRef path, llvm::StringRef name);
  static std::string library(llvm::StringRef path, llvm::StringRef name, llvm::StringRef hint);
  static std::string unmarked(llvm::StringRef path, llvm::StringRef name, llvm::StringRef file);
};

class ErrorTextExportSource {
public:
  static std::string neverthrow();
  static std::string builtin(llvm::StringRef path);
};

class ErrorTextLocalExport {
public:
  static std::string builtin(llvm::StringRef name);
  static std::string notFound(llvm::StringRef name);
};

class ErrorTextFreeExport {
public:
  static std::string previous();
  static std::string duplicate(llvm::StringRef alias);
};

class ErrorTextStarExport {
public:
  static std::string twice(llvm::StringRef name, llvm::StringRef first, llvm::StringRef second);
};

class ErrorTextWholeImport {
public:
  static std::string byDefault(llvm::StringRef path);
  static std::string star(llvm::StringRef path);
};

class ErrorTextFreeName {
public:
  static std::string previous();
  static std::string duplicate(llvm::StringRef alias);
  static std::string declared(llvm::StringRef alias);
  static std::string conflicts(llvm::StringRef alias, llvm::StringRef name, llvm::StringRef path);
};

class ErrorTextNotLoaded {
public:
  static std::string whileLoads(llvm::StringRef reader, llvm::StringRef what,
                                llvm::StringRef source, llvm::StringRef chain,
                                llvm::StringRef declared);
  static std::string beforeCall(llvm::StringRef what, llvm::StringRef reader,
                                llvm::StringRef early);
  static std::string inFunction(llvm::StringRef what, llvm::StringRef reader);
  static std::string label(llvm::StringRef name, llvm::StringRef declared, llvm::StringRef file);
  static std::string labelOf(llvm::StringRef name, llvm::StringRef file);
  static std::string quoted(llvm::StringRef name);
};

enum class DeclarationModulesRegistryImportSourceErrorCode {
  NEVERTHROW_IMPORT,
};

struct DeclarationModulesRegistryImportSourceError {
  DeclarationModulesRegistryImportSourceErrorCode code;
  std::string message;
};

class ErrorTextImportSource {
public:
  static std::string neverthrow();
};

}
