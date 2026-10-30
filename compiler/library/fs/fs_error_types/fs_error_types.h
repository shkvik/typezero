#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FsErrorTypes {
public:
  explicit FsErrorTypes(Checker &checker);

  void declare();
  const Type *named(llvm::StringRef name);
  const Type *ofCall(const CallExpr *call);
  static llvm::StringRef verbOf(llvm::StringRef name);

private:
  Checker &checker_;
};

}
