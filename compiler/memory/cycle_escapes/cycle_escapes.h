#pragma once

#include "compiler/memory/cycle_scanner/cycle_scanner.h"

namespace tz {

class CycleEscapes {
public:
  explicit CycleEscapes(CycleScanner &scanner);

  static void ordered(const Stmt *stmt, const VarSymbol *self, bool &escaped, bool afterSuper,
                      llvm::DenseSet<const AssignExpr *> &safe);
  static void orderedExpr(const Expr *e, const VarSymbol *self, bool &escaped, bool afterSuper,
                          llvm::DenseSet<const AssignExpr *> &safe);
  static bool exprEscapes(const Expr *expr, const VarSymbol *self);
  bool constructionEscapes(const ClassDecl *cls);
  static bool mentions(const Expr *expr, const VarSymbol *self);
  static bool mentionsIn(const Stmt *stmt, const VarSymbol *self);
  static void forEachChild(const Expr *e, const std::function<void(const Expr *)> &visit);
  static void walkStmt(const Stmt *stmt, const std::function<void(const Expr *)> &visit);

private:
  CycleScanner &scanner_;
};

}
