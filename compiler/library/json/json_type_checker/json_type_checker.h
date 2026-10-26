#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class JsonTypeChecker {
public:
  explicit JsonTypeChecker(Checker &checker);

  bool checkFromJsonType(const Type *type, llvm::SMLoc loc, llvm::StringRef fn);
  static std::string jsonProblem(const Type *type, Checker::ClassProblem classProblem);

private:
  Checker &checker_;
};

}
