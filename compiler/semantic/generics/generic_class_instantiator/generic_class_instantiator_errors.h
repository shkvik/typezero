#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class GenericClassInstantiatorInstantiateClassErrorCode {
  ALREADY_REPORTED,
  TOO_MANY_TYPE_ARGUMENTS,
  EVENT_MAP_MISSING,
  TYPE_ARGUMENTS_MISSING,
  CHUNK_TYPE_MISSING,
  UNDEFINED_TYPE_ARGUMENT,
  CONSTRAINT_UNSATISFIED,
  ENDLESS_INSTANCES,
};

struct GenericClassInstantiatorInstantiateClassError {
  GenericClassInstantiatorInstantiateClassErrorCode code;
  std::string message;
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
