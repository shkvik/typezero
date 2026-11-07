#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ChildChecker {
public:
  explicit ChildChecker(Checker &owner);

  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  static std::string utf8(const std::u16string &text);
  static const Type *bufferType();
  static bool knownSignal(llvm::StringRef name);
  static bool hasZero(const Expr *expr);

  Checker &checker;
};

}
