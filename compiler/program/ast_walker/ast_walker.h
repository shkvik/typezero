#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/StringSet.h"
#include <vector>

namespace tz {

struct AstPart {
  const Stmt *stmt;
  const Expr *expr;
};

struct NamePart {
  const Stmt *stmt;
  const Expr *expr;
  bool inFunction;
};

struct TailPart {
  Stmt *stmt;
  bool tail;
};

class AstWalker {
public:
  static void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction = true);
  static void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction = true);
  static std::vector<const Expr *> ownExprsIn(const Stmt *stmt);
  static std::vector<const Expr *> ownExprsIn(const Expr *expr);
  static std::vector<const Expr *> allExprsIn(const Stmt *stmt);
  static std::vector<const Expr *> allExprsIn(const Expr *expr);
  static std::vector<CallExpr *> tailCallsOf(Expr *value);
  static std::vector<Stmt *> tailStatementsOf(Stmt *body);
  static std::vector<CallExpr *> tailCallsIn(FunctionDecl *fn);

private:
  static void namesFrom(NamePart root, llvm::StringSet<> &names);
  static void addReadName(const NamePart &part, llvm::StringSet<> &names);
  static void addNameChildren(const Stmt *stmt, bool inFunction, std::vector<NamePart> &stack);
  static void addNameChildren(const Expr *expr, bool inFunction, std::vector<NamePart> &stack);
  static std::vector<const Expr *> exprsFrom(AstPart root, bool intoFunctions);
  static void addExprChildren(const Expr *expr, bool intoFunctions, std::vector<AstPart> &children);
  static void addStmtChildren(const Stmt *stmt, bool intoFunctions, std::vector<AstPart> &children);
  static void addClassChildren(const ClassDecl *cls, bool intoFunctions,
                               std::vector<AstPart> &children);
  static void addDecoratorChildren(const FunctionDecl *fn, std::vector<AstPart> &children);
  static std::vector<Expr *> returnValuesIn(Stmt *body);
  static std::vector<Stmt *> returnScopeChildren(Stmt *stmt);
  static bool isTailAt(const BlockStmt *block, size_t i, bool tail);
  static bool isPlainCallStatement(const Stmt *stmt);
};

}
