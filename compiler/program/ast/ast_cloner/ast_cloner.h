#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include <utility>
#include <vector>

namespace tz {

class AstCloner {
public:
  explicit AstCloner(AstContext &target);

  ClassDecl *copyClass(const ClassDecl *cls);
  FunctionDecl *copyFunction(const FunctionDecl *fn);
  Stmt *copyStatement(const Stmt *stmt);
  void keepPristine(llvm::ArrayRef<Stmt *> items);

private:
  Expr *cloneExpr(const Expr *expr);
  Stmt *cloneStmt(const Stmt *stmt);
  void cloneParam(Param &param);
  void cloneField(ClassField &field);
  void finishCopies(bool snapshot);
  ClassDecl *cloneClass(const ClassDecl *cls);
  BlockStmt *cloneBlock(const BlockStmt *block);
  void cloneSwitchCase(SwitchCase &switchCase);
  void cloneDeclarator(VarDeclarator &declarator);
  llvm::StringRef ownName(llvm::StringRef name);
  llvm::StringRef usedName(llvm::StringRef name) const;
  FunctionDecl *cloneFunction(const FunctionDecl *fn);
  void cloneStatements(std::vector<Stmt *> &statements);
  void cloneProperty(ObjectLitExpr::Property &property);
  void cloneDecorators(std::vector<Decorator> &decorators);
  void cloneExpressions(std::vector<Expr *> &expressions);
  static bool isHiddenName(llvm::StringRef name);

  AstContext &context;
  bool renames;
  llvm::DenseMap<const Node *, Node *> copies;
  llvm::StringMap<llvm::StringRef> hiddenNames;
  std::vector<SpreadProperty *> spreads;
  std::vector<std::pair<const ClassDecl *, ClassDecl *>> classes;
  std::vector<std::pair<const FunctionDecl *, FunctionDecl *>> functions;
};

}
