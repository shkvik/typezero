#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextRuntimeObject {
public:
  static std::string members(Type::Runtime kind);
  static std::string noMember(llvm::StringRef name, llvm::StringRef type, Type::Runtime kind);
  static std::string assigned(llvm::StringRef name, llvm::StringRef type, Type::Runtime kind);
};

class ErrorTextReadlineMethod {
public:
  static std::string callback();
  static std::string options();
  static std::string events(llvm::StringRef name);
};

class ErrorTextAsyncIterator {
public:
  static std::string noThrow();
  static std::string notYet(llvm::StringRef name);
};

}
