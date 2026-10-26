#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

class ProgramScan {
public:
  explicit ProgramScan(OverflowCheck &check);

  void scanProgram();
  void addFunction(const FunctionDecl *fn);
  void scanFunction(const FunctionDecl *fn);
  void scanClass(const ClassDecl *cls);
  void declareVar(const VarSymbol *var, const VarDeclarator *decl = nullptr, bool isConst = false);
  void scanStmt(const Stmt *stmt);
  void scanExpr(const Expr *expr, bool consumed);
  void useVar(const VarSymbol *var, bool consumed, bool write);
  bool isConstantTree(const Expr *expr) const;

private:
  OverflowCheck &check_;
};

}
