#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GenericsAliasesRegistryArgumentCountErrorCode {
  TOO_MANY_TYPE_ARGUMENTS,
};

struct GenericsAliasesRegistryArgumentCountError {
  GenericsAliasesRegistryArgumentCountErrorCode code;
  std::string message;
};

enum class GenericsAliasesRegistryDefaultExistsErrorCode {
  TYPE_ARGUMENTS_MISSING,
};

struct GenericsAliasesRegistryDefaultExistsError {
  GenericsAliasesRegistryDefaultExistsErrorCode code;
  std::string message;
};

enum class GenericsAliasesRegistryNotResolvingErrorCode {
  REFERS_TO_ITSELF,
};

struct GenericsAliasesRegistryNotResolvingError {
  GenericsAliasesRegistryNotResolvingErrorCode code;
  std::string message;
};

enum class GenericsAliasesRegistryDepthInBoundsErrorCode {
  ENDLESS_ALIAS,
  TOO_MANY_TAIL_CALLS,
  TOO_DEEP,
};

struct GenericsAliasesRegistryDepthInBoundsError {
  GenericsAliasesRegistryDepthInBoundsErrorCode code;
  std::string message;
};

enum class GenericsAliasesRegistryArgumentMatchErrorCode {
  CONSTRAINT_UNSATISFIED,
};

struct GenericsAliasesRegistryArgumentMatchError {
  GenericsAliasesRegistryArgumentMatchErrorCode code;
  std::string message;
};

class ErrorTextAliasCount {
public:
  static std::string tooMany(llvm::StringRef name, size_t count, size_t got);
};

class ErrorTextAliasDefault {
public:
  static std::string needs(llvm::StringRef name, size_t count);
};

class ErrorTextAliasMatch {
public:
  static std::string unsatisfied(llvm::StringRef arg, llvm::StringRef constraint,
                                 llvm::StringRef param, llvm::StringRef name);
};

class ErrorTextAliasSelf {
public:
  static std::string itself(llvm::StringRef name);
};

class ErrorTextAliasDepth {
public:
  static std::string endless(llvm::StringRef name);
  static std::string tailCalls(llvm::StringRef name, unsigned most);
  static std::string nests(llvm::StringRef name, unsigned depth, unsigned calls);
  static std::string neededHere();
};

}
