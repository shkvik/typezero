#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringSet.h"
#include <vector>

namespace tz {

class AstWalker {
public:
  static void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction = true);
  static void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction = true);
  static void collectMemberCalls(const Stmt *body, llvm::StringSet<> &names);
  static void visitExprs(const Stmt *stmt, llvm::function_ref<void(const Expr *)> visit,
                         bool intoFunctions = true);
  static void visitExprs(const Expr *expr, llvm::function_ref<void(const Expr *)> visit,
                         bool intoFunctions = true);
  static void tailCallsOf(Expr *value, std::vector<CallExpr *> &calls);
  static void tailStatementsOf(Stmt *stmt, bool tail, std::vector<Stmt *> &stmts);
  static std::vector<CallExpr *> tailCallsIn(FunctionDecl *fn);

private:
  static void forEachReturnValue(Stmt *stmt, llvm::function_ref<void(Expr *)> visit);
};

}
