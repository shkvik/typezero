#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The types JSON.parse<T> can read.
class JsonTypeChecker {
public:
  explicit JsonTypeChecker(Checker &checker);

  // The type T of JSON.parse<T> and fromJson<T>, which the compiler can
  // build from a Json; reports the part of it that it cannot.
  bool checkFromJsonType(const Type *type, llvm::SMLoc loc, llvm::StringRef fn);
  static std::string jsonProblem(const Type *type, Checker::ClassProblem classProblem);

private:
  Checker &checker_;
};

} // namespace tz
