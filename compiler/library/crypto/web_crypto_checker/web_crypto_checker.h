#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class WebCryptoChecker {
public:
  explicit WebCryptoChecker(Checker &checker);

  Expr *checkSubtleIntrinsic(CallExpr *call, NameExpr *name);
  Expr *subtleObject(MemberExpr *member);
  bool isSubtleClass(const Type *type, llvm::StringRef name) const;

private:
  Checker &checker_;
};

}
