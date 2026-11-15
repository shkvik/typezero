#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryFsRegistryOctalModeErrorCode {
  NOT_OCTAL,
};

struct LibraryFsRegistryOctalModeError {
  LibraryFsRegistryOctalModeErrorCode code;
  std::string message;
};

class ErrorTextFsEncoding {
public:
  static std::string literal(llvm::StringRef fn);
  static std::string utf8Only(llvm::StringRef fn, llvm::StringRef encoding);
  static std::string dependsOn(llvm::StringRef fn);
  static std::string forText(llvm::StringRef fn);
};

class ErrorTextFsCall {
public:
  static std::string notFunc(llvm::StringRef fn);
  static std::string zeroPath();
  static std::string flag(llvm::StringRef fn, llvm::StringRef written, bool literal);
  static std::string data(llvm::StringRef fn, const Type *type);
  static std::string chmod();
  static std::string bigint();
  static std::string rmdir();
  static std::string copyMode(llvm::StringRef fn);
};

class ErrorTextFsConstant {
public:
  static std::string value();
  static std::string notYet(llvm::StringRef name);
};

enum class LibraryFsRegistryPromiseFunctionErrorCode {
  NOT_A_FUNCTION,
  NO_FILE_HANDLE,
};

struct LibraryFsRegistryPromiseFunctionError {
  LibraryFsRegistryPromiseFunctionErrorCode code;
  std::string message;
};

class ErrorTextFsPromises {
public:
  static std::string constants();
  static std::string open();
  static std::string notYet(llvm::StringRef name);
};

enum class LibraryFsRegistryReadlineFunctionErrorCode {
  TYPE_ONLY,
};

struct LibraryFsRegistryReadlineFunctionError {
  LibraryFsRegistryReadlineFunctionErrorCode code;
  std::string message;
};

enum class LibraryFsRegistryStdinInputErrorCode {
  NOT_STDIN,
};

struct LibraryFsRegistryStdinInputError {
  LibraryFsRegistryStdinInputErrorCode code;
  std::string message;
};

class ErrorTextReadline {
public:
  static std::string typeOnly(llvm::StringRef fn, bool promises);
  static std::string option(llvm::StringRef name);
  static std::string noInput();
  static std::string notStdin();
  static std::string output();
};

}
