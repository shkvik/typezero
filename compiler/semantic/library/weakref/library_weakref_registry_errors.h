#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryWeakrefRegistryTargetWrittenErrorCode {
  NO_TARGET_TYPE,
};

struct LibraryWeakrefRegistryTargetWrittenError {
  LibraryWeakrefRegistryTargetWrittenErrorCode code;
  std::string message;
};

enum class LibraryWeakrefRegistryWeakTargetErrorCode {
  NOT_SUPPORTED_YET,
  NOT_AN_OBJECT,
};

struct LibraryWeakrefRegistryWeakTargetError {
  LibraryWeakrefRegistryWeakTargetErrorCode code;
  std::string message;
};

enum class LibraryWeakrefRegistryNewArgsErrorCode {
  NOT_ONE_OBJECT,
};

struct LibraryWeakrefRegistryNewArgsError {
  LibraryWeakrefRegistryNewArgsErrorCode code;
  std::string message;
};

enum class LibraryWeakrefRegistryNewTypeArgsErrorCode {
  SEVERAL_TYPE_ARGUMENTS,
};

struct LibraryWeakrefRegistryNewTypeArgsError {
  LibraryWeakrefRegistryNewTypeArgsErrorCode code;
  std::string message;
};

enum class LibraryWeakrefRegistryMethodNameErrorCode {
  NO_SUCH_METHOD,
};

struct LibraryWeakrefRegistryMethodNameError {
  LibraryWeakrefRegistryMethodNameErrorCode code;
  std::string message;
};

class ErrorTextWeakRef {
public:
  static std::string noTarget();
  static std::string oneObject();
  static std::string typeArgs();
  static std::string noMethod(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextWeakTarget {
public:
  static std::string notYet(llvm::StringRef type);
  static std::string notObject(llvm::StringRef type, bool optional);
};

}
