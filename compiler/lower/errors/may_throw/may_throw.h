#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Which functions and calls may throw.
class MayThrow {
public:
  explicit MayThrow(CodeGen &codegen);

  // Exceptions (codegen_throw.cpp, docs/errors.md). The error in flight is
  // the runtime's tz_thrown: a function that throws stores its Error there
  // and returns, releasing its variables on the way out, and the caller of a
  // function that may throw checks tz_thrown after the call. Which functions
  // may throw is found over the whole program (analyzeThrows): the others,
  // and their callers, pay nothing.
  void analyzeThrows(SourceFile &file);
  // A call of the method `fn`; through the method table, any method that
  // overrides it down the chain may run.
  bool callMayThrow(const FunctionDecl *fn, bool virtualCall) const;
  // Evaluating `expr` may throw.
  bool exprThrows(const Expr *expr);
  bool summaryThrows(const ThrowSummary &summary) const;

private:
  CodeGen &codegen_;
  llvm::DenseMap<const Expr *, bool> exprThrows_;
};

} // namespace tz
