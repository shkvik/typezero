#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ChildChecker {
public:
  explicit ChildChecker(CheckState &owner);

  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  static std::string utf8(const std::u16string &text);
  static const Type *bufferType();
  static bool knownSignal(llvm::StringRef name);
  static bool hasZero(const Expr *expr);

  CheckState &state;
};

}
