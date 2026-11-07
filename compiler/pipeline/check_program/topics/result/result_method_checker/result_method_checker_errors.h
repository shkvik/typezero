#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextResultMethod {
public:
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string unwrapOr(llvm::StringRef value, llvm::StringRef other);
  static std::string function(llvm::StringRef name, llvm::StringRef type);
  static std::string match(llvm::StringRef a, llvm::StringRef b);
  static std::string result(llvm::StringRef name, llvm::StringRef type, bool andThen);
  static std::string errors(const Type *a, llvm::StringRef name, const Type *b);
  static std::string values(const Type *value, const Type *other);
};

}
