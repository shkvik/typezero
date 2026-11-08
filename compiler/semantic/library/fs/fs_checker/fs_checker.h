#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FsChecker {
public:
  explicit FsChecker(SemanticState &owner);

  Expr *checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkTzIoCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkFsValue(llvm::StringRef name, Expr *expr);
  Expr *checkFsConstant(MemberExpr *member);
  bool nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                   llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options);
  Expr *checkEncoding(Expr *encoding, llvm::StringRef fn, bool any);
  const Type *nativeError() const;

private:
  using ResultOctalMode = Result<unsigned, FsCheckerOctalModeError>;

  static ResultOctalMode octalMode(llvm::StringRef text);
  static std::u16string utf16(llvm::StringRef text);

  SemanticState &state;
};

}
