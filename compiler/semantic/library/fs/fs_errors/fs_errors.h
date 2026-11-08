#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <string>

namespace tz {

class FsErrors {
public:
  explicit FsErrors(SemanticState &owner);

  void declare();
  const Type *named(llvm::StringRef name);
  const Type *ofCall(const CallExpr *call);

private:
  static std::string textOf(const Expr *expr);
  static bool literalTrue(const Expr *expr);

  SemanticState &state;
};

}
