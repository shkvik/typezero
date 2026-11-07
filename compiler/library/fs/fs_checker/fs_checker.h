#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FsChecker {
public:
  explicit FsChecker(Checker &owner);

  Expr *checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkTzIoCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkFsValue(llvm::StringRef name, Expr *expr);
  Expr *checkFsConstant(MemberExpr *member);
  bool nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                   llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options);
  Expr *checkEncoding(Expr *encoding, llvm::StringRef fn, bool any);
  const Type *nativeError() const;

private:
  static std::u16string utf16(llvm::StringRef text);
  static std::string utf8(const std::u16string &text);

  Checker &checker;
};

}
