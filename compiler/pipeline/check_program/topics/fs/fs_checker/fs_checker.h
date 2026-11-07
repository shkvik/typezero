#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class FsChecker {
public:
  explicit FsChecker(CheckState &owner);

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

  CheckState &state;
};

}
