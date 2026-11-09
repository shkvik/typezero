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
  LoopScan scanForOfLoop(const ForOfStmt *loop) const;
  bool namesIterator(const Expr *expr) const;
  void scanStmt(const Stmt *stmt, LoopScan &scan, unsigned depth) const;
  void scanExpr(const Expr *expr, LoopScan &scan, unsigned depth) const;

private:
  static void declareParams(const FunctionDecl *fn, LoopScan &scan);
  static bool passesNamedFunction(const CallExpr *call, int callbackArgs);
  const Type *getTypeBeforeLoop(const Expr *expr, const LoopScan &scan) const;
  static void scanArrayMethod(const CallExpr *call, llvm::StringRef name, LoopScan &scan);
  static void scanResultMethod(const CallExpr *call, llvm::StringRef name, LoopScan &scan);
  void scanMethodByName(const CallExpr *call, const MemberExpr *member, LoopScan &scan) const;
  void scanMethodCall(const CallExpr *call, const MemberExpr *member, LoopScan &scan,
                      unsigned depth) const;
  LoopScan scanParts(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs,
                     const llvm::StringSet<> &declaredInside) const;
  LoopScan scanWithNames(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs,
                         const llvm::StringSet<> &headNames) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
