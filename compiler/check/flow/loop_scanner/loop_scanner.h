#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// What the body of a loop changes.
class LoopScanner {
public:
  explicit LoopScanner(Sema &sema);

  Sema::LoopScan scanLoop(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs) const;
  bool namesIterator(const Expr *expr) const;
  void scanStmt(const Stmt *stmt, Sema::LoopScan &scan, unsigned depth) const;
  void scanExpr(const Expr *expr, Sema::LoopScan &scan, unsigned depth) const;

private:
  Sema &sema_;
};

} // namespace tz
