#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GenericsClassesRegistryDeclaredFirstErrorCode {
  USED_BEFORE_DECLARATION,
};

struct GenericsClassesRegistryDeclaredFirstError {
  GenericsClassesRegistryDeclaredFirstErrorCode code;
  std::string message;
};

enum class GenericsClassesRegistryConcreteErrorCode {
  ABSTRACT_CLASS,
};

struct GenericsClassesRegistryConcreteError {
  GenericsClassesRegistryConcreteErrorCode code;
  std::string message;
};

enum class GenericsClassesRegistryConstructorFitErrorCode {
  NO_CONSTRUCTOR_FITS,
  SEVERAL_CONSTRUCTORS_FIT,
};

struct GenericsClassesRegistryConstructorFitError {
  GenericsClassesRegistryConstructorFitErrorCode code;
  std::string message;
};

enum class GenericsClassesRegistryInstantiateClassErrorCode {
  ALREADY_REPORTED,
  TOO_MANY_TYPE_ARGUMENTS,
  EVENT_MAP_MISSING,
  TYPE_ARGUMENTS_MISSING,
  CHUNK_TYPE_MISSING,
  UNDEFINED_TYPE_ARGUMENT,
  CONSTRAINT_UNSATISFIED,
  ENDLESS_INSTANCES,
};

struct GenericsClassesRegistryInstantiateClassError {
  GenericsClassesRegistryInstantiateClassErrorCode code;
  std::string message;
};

class ErrorTextDeclaredFirst {
public:
  static std::string before(llvm::StringRef name);
};

class ErrorTextConcrete {
public:
  static std::string abstract(llvm::StringRef name);
};

class ErrorTextConstructorFit {
public:
  static std::string inferArgs(llvm::StringRef name, bool none, size_t count);
};

class ErrorTextClassInstance {
public:
  static std::string tooMany(llvm::StringRef name, size_t count, size_t got);
  static std::string eventMap();
  static std::string needs(llvm::StringRef name, size_t count);
  static std::string chunks(llvm::StringRef name);
  static std::string undefinedArg(llvm::StringRef arg, llvm::StringRef name);
  static std::string unsatisfied(llvm::StringRef arg, llvm::StringRef constraint,
                                 llvm::StringRef param, llvm::StringRef name);
  static std::string endless(llvm::StringRef name);
};

}
