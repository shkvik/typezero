#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextInstanceUse {
public:
  static std::string cannotUse(llvm::StringRef name, llvm::StringRef bindings,
                               llvm::StringRef message);
  static std::string usesHere(llvm::StringRef name, llvm::StringRef next);
  static std::string here(llvm::StringRef name);
};

enum class GenericClassCheckerDeclaredFirstErrorCode {
  USED_BEFORE_DECLARATION,
};

struct GenericClassCheckerDeclaredFirstError {
  GenericClassCheckerDeclaredFirstErrorCode code;
  std::string message;
};

class ErrorTextDeclaredFirst {
public:
  static std::string before(llvm::StringRef name);
};

enum class GenericClassCheckerConcreteErrorCode {
  ABSTRACT_CLASS,
};

struct GenericClassCheckerConcreteError {
  GenericClassCheckerConcreteErrorCode code;
  std::string message;
};

class ErrorTextConcrete {
public:
  static std::string abstract(llvm::StringRef name);
};

enum class GenericClassCheckerConstructorFitErrorCode {
  NO_CONSTRUCTOR_FITS,
  SEVERAL_CONSTRUCTORS_FIT,
};

struct GenericClassCheckerConstructorFitError {
  GenericClassCheckerConstructorFitErrorCode code;
  std::string message;
};

class ErrorTextConstructorFit {
public:
  static std::string inferArgs(llvm::StringRef name, bool none, size_t count);
};

}
