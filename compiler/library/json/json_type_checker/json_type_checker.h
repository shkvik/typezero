#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The types JSON.parse<T> can read.
class JsonTypeChecker {
public:
  explicit JsonTypeChecker(Sema &sema);

  // The type T of JSON.parse<T> and fromJson<T>, which the compiler can
  // build from a Json; reports the part of it that it cannot.
  bool checkFromJsonType(const Type *type, llvm::SMLoc loc, llvm::StringRef fn);
  static std::string jsonProblem(const Type *type, Sema::ClassProblem classProblem);

private:
  Sema &sema_;
};

} // namespace tz
