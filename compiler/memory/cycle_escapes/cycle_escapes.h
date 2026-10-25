#pragma once

#include "compiler/memory/cycle_scanner/cycle_scanner.h"

namespace tz {

// Whether `this` escapes a constructor before the object is complete.
class CycleEscapes {
public:
  explicit CycleEscapes(CycleScanner &scanner);

  // Walks `stmt` in the order it runs, noting the writes to fields of `self`
  // made while `escaped` is still false; loops, `switch` and `try` count as
  // a whole.
  static void ordered(const Stmt *stmt, const VarSymbol *self, bool &escaped, bool afterSuper,
                      llvm::DenseSet<const AssignExpr *> &safe);
  static void orderedExpr(const Expr *e, const VarSymbol *self, bool &escaped, bool afterSuper,
                          llvm::DenseSet<const AssignExpr *> &safe);
  static bool exprEscapes(const Expr *expr, const VarSymbol *self);
  // Whether the construction of an object of `cls` lets `this` go anywhere
  // before it ends: anything but writing and reading its fields, there or
  // in the classes up the chain.
  bool constructionEscapes(const ClassDecl *cls);
  static bool mentions(const Expr *expr, const VarSymbol *self);
  static bool mentionsIn(const Stmt *stmt, const VarSymbol *self);
  // The expressions directly in `e`.
  static void forEachChild(const Expr *e, const std::function<void(const Expr *)> &visit);
  // The expressions of `stmt` and of the statements in it (not of the
  // functions it declares).
  static void walkStmt(const Stmt *stmt, const std::function<void(const Expr *)> &visit);

private:
  CycleScanner &scanner_;
};

} // namespace tz
