#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class MayThrow {
public:
  explicit MayThrow(Lowering &lowering);

  void analyzeThrows(SourceFile &file);
  bool callMayThrow(const FunctionDecl *fn, bool virtualCall) const;
  bool exprThrows(const Expr *expr);
  bool summaryThrows(const ThrowSummary &summary) const;

private:
  Lowering &lowering_;
  llvm::DenseMap<const Expr *, bool> exprThrows_;
};

}
