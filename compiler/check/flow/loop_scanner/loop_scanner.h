#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// What the body of a loop changes.
class LoopScanner {
public:
  explicit LoopScanner(Checker &checker);

  Checker::LoopScan scanLoop(llvm::ArrayRef<const Stmt *> stmts,
                             llvm::ArrayRef<const Expr *> exprs) const;
  bool namesIterator(const Expr *expr) const;
  void scanStmt(const Stmt *stmt, Checker::LoopScan &scan, unsigned depth) const;
  void scanExpr(const Expr *expr, Checker::LoopScan &scan, unsigned depth) const;

private:
  Checker &checker_;
};

} // namespace tz
