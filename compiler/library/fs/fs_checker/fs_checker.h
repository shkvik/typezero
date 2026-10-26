#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FsChecker {
public:
  explicit FsChecker(Checker &checker);

  Expr *checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  bool nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                   llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options);
  Expr *checkEncoding(Expr *encoding, llvm::StringRef fn, bool any);

private:
  Checker &checker_;
};

}
