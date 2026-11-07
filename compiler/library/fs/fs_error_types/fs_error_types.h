#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FsErrorTypes {
public:
  explicit FsErrorTypes(Checker &owner);

  void declare();
  const Type *named(llvm::StringRef name);
  const Type *ofCall(const CallExpr *call);
  static llvm::StringRef verbOf(llvm::StringRef name);

private:
  static std::string textOf(const Expr *expr);
  static bool literalTrue(const Expr *expr);

  Checker &checker;
};

}
