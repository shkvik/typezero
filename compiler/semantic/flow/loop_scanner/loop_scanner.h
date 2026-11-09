#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class KeyedFieldChecker;

class LoopScanner {
public:
  LoopScanner(SemanticState &owner, SemanticParts &all);

  LoopScan scanLoop(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs) const;
  bool namesIterator(const Expr *expr) const;
  void scanStmt(const Stmt *stmt, LoopScan &scan, unsigned depth) const;
  void scanExpr(const Expr *expr, LoopScan &scan, unsigned depth) const;

private:
  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
