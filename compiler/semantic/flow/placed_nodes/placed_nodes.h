#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/semantic/flow/tree_place/tree_place.h"
#include "llvm/ADT/ArrayRef.h"
#include <vector>

namespace tz {

struct PlacedStmt {
  const Stmt *stmt;
  TreePlace place;
};

struct PlacedExpr {
  const Expr *expr;
  TreePlace place;
};

class PlacedNodes {
public:
  static PlacedNodes collect(llvm::ArrayRef<const Stmt *> stmts,
                             llvm::ArrayRef<const Expr *> exprs);

  llvm::ArrayRef<PlacedStmt> getStmts() const;
  llvm::ArrayRef<PlacedExpr> getExprs() const;

private:
  void addStmtChildren(size_t &nextStmt);
  void addExprChildren(size_t &nextExpr);
  void addStmt(const Stmt *stmt, const TreePlace &place);
  void addExpr(const Expr *expr, const TreePlace &place);
  void addChildren(const Stmt *stmt, const TreePlace &place);
  void addChildren(const Expr *expr, const TreePlace &place);
  void addLoopParts(const Stmt *stmt, const TreePlace &place);
  void addCallParts(const Expr *expr, const TreePlace &place);
  void addListParts(const Expr *expr, const TreePlace &place);
  void addPlainParts(const Stmt *stmt, const TreePlace &place);
  void addBranchParts(const Stmt *stmt, const TreePlace &place);
  void addSwitchParts(const SwitchStmt *switchStmt, const TreePlace &place);
  void addCaseParts(const SwitchCase &branch, const TreePlace &place);
  void addNewParts(const NewExpr *made, const TreePlace &place);
  void addAccessParts(const Expr *expr, const TreePlace &place);
  void addOperatorParts(const Expr *expr, const TreePlace &place);
  void addNestedCodeParts(const Expr *expr, const TreePlace &place);
  void addDeclarationParts(const Stmt *stmt, const TreePlace &place);
  void addExprs(llvm::ArrayRef<Expr *> list, const TreePlace &place);
  void addClassChildren(const ClassDecl *classDecl, const TreePlace &place);
  void addFieldParts(const ClassField &field, const TreePlace &place);
  void addFunctionChildren(const FunctionDecl *functionDecl, const TreePlace &place);
  void addParamParts(const Param &param, const TreePlace &place);

  std::vector<PlacedStmt> stmts;
  std::vector<PlacedExpr> exprs;
};

}
