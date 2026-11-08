#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class Destination {
public:
  explicit Destination(SemanticState &owner);

  const Type *literalDestination(const VarDeclarator &d, llvm::ArrayRef<VarDeclarator> later);
  static bool literalShaped(const Expr *expr);
  bool literalFits(const Expr *expr, const Type *type) const;
  const Type *declaredFunctionType(const FunctionDecl *fn);

private:
  static void forEachReturn(const Stmt *stmt, llvm::function_ref<void(const Expr *)> each);
  static const Type *writable(const Type *type);
  SemanticState &state;
};

}
