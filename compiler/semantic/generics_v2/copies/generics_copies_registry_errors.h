#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextInstanceCall {
public:
  static std::string cannotCall(llvm::StringRef name, llvm::StringRef bindings,
                                llvm::StringRef message);
  static std::string inWhich(llvm::StringRef name, bool uses, llvm::StringRef next);
  static std::string here(llvm::StringRef name);
};

class ErrorTextInstanceUse {
public:
  static std::string cannotUse(llvm::StringRef name, llvm::StringRef bindings,
                               llvm::StringRef message);
  static std::string usesHere(llvm::StringRef name, llvm::StringRef next);
  static std::string here(llvm::StringRef name);
};

}
