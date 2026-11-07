#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class LoopScanner {
public:
  explicit LoopScanner(CheckState &owner);

  LoopScan scanLoop(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs) const;
  bool namesIterator(const Expr *expr) const;
  void scanStmt(const Stmt *stmt, LoopScan &scan, unsigned depth) const;
  void scanExpr(const Expr *expr, LoopScan &scan, unsigned depth) const;

private:
  CheckState &state;
};

}
