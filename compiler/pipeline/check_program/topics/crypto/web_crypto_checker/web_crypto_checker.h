#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class WebCryptoChecker {
public:
  explicit WebCryptoChecker(CheckState &owner);

  Expr *checkSubtleIntrinsic(CallExpr *call, NameExpr *name, llvm::StringRef intrinsicName);
  Expr *subtleObject(MemberExpr *member);
  bool isSubtleClass(const Type *type, llvm::StringRef name) const;

private:
  CheckState &state;
};

}
