#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class FsErrorTypes {
public:
  explicit FsErrorTypes(CheckState &owner);

  void declare();
  const Type *named(llvm::StringRef name);
  const Type *ofCall(const CallExpr *call);
  static llvm::StringRef verbOf(llvm::StringRef name);

private:
  static std::string textOf(const Expr *expr);
  static bool literalTrue(const Expr *expr);

  CheckState &state;
};

}
