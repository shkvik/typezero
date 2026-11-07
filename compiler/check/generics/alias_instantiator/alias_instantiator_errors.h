#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class AliasInstantiatorArgumentCountErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
};

struct AliasInstantiatorArgumentCountError {
  AliasInstantiatorArgumentCountErrorCode code;
  std::string message;
};

class ErrorTextAliasCount {
public:
  static std::string tooMany(llvm::StringRef name, size_t count, size_t got);
};

enum class AliasInstantiatorDefaultExistsErrorCode {
  TYPE_ARGUMENTS_MISSING,
};

struct AliasInstantiatorDefaultExistsError {
  AliasInstantiatorDefaultExistsErrorCode code;
  std::string message;
};

class ErrorTextAliasDefault {
public:
  static std::string needs(llvm::StringRef name, size_t count);
};

enum class AliasInstantiatorArgumentMatchErrorCode {
  CONSTRAINT_UNSATISFIED,
};

struct AliasInstantiatorArgumentMatchError {
  AliasInstantiatorArgumentMatchErrorCode code;
  std::string message;
};

class ErrorTextAliasMatch {
public:
  static std::string unsatisfied(llvm::StringRef arg, llvm::StringRef constraint,
                                 llvm::StringRef param, llvm::StringRef name);
};

enum class AliasInstantiatorNotResolvingErrorCode {
  REFERS_TO_ITSELF,
};

struct AliasInstantiatorNotResolvingError {
  AliasInstantiatorNotResolvingErrorCode code;
  std::string message;
};

class ErrorTextAliasSelf {
public:
  static std::string itself(llvm::StringRef name);
};

enum class AliasInstantiatorDepthInBoundsErrorCode {
  ENDLESS_ALIAS,
  TOO_MANY_TAIL_CALLS,
  TOO_DEEP,
};

struct AliasInstantiatorDepthInBoundsError {
  AliasInstantiatorDepthInBoundsErrorCode code;
  std::string message;
};

class ErrorTextAliasDepth {
public:
  static std::string endless(llvm::StringRef name);
  static std::string tailCalls(llvm::StringRef name, unsigned most);
  static std::string nests(llvm::StringRef name, unsigned depth, unsigned calls);
  static std::string neededHere();
};

}
