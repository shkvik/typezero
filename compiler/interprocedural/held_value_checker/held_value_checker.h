#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/program/program.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <functional>
#include <set>
#include <utility>
#include <vector>

namespace tz {

class HeldValueChecker {
public:
  explicit HeldValueChecker(Program &checked);
  virtual ~HeldValueChecker() = default;

  void run();

protected:
  static const Expr *bare(const Expr *e);
  static bool storageMethod(const CallExpr *c, llvm::StringRef name);
  static const VarSymbol *storageOf(const CallExpr *c);
  static const FunctionDecl *callbackOf(const Expr *e);
  static const ClassDecl *storageClass(const ClassDecl *cls);

  virtual bool used() const = 0;
  virtual void kept(const Expr *value, bool closure, llvm::StringRef where) = 0;
  virtual bool produces(const CallExpr *c) const;
  virtual bool takesOver(const CallExpr *c);
  virtual void sawNew(const NewExpr *n);
  virtual void sawVoid(const CallExpr *c);
  virtual void sawCall(const CallExpr *c);
  virtual bool heldMember(const MemberExpr *m) const;
  virtual void spreadKept(const Expr *value);
  virtual size_t extraKnown() const;
  virtual bool closuresCarry() const;
  virtual void finish();
  virtual void startWalk();

  bool once(const Expr *value);
  bool isHeld(const Expr *e) const;
  bool inLibrary(llvm::SMLoc loc) const;
  bool usesModule(llvm::StringRef name) const;
  bool carries(const FunctionDecl *fn) const;
  void markThis(const FunctionDecl *fn);
  void markParam(const FunctionDecl *fn, unsigned index);
  const FunctionDecl *programTarget(const CallExpr *c) const;

  Program &program;
  const FunctionDecl *current;
  llvm::DenseSet<const Expr *> awaited;
  llvm::DenseSet<const Expr *> allowedNews;
  llvm::DenseSet<const Expr *> allowedStores;
  llvm::DenseSet<const VarSymbol *> heldVars;
  llvm::DenseSet<const FunctionDecl *> returnsHeld;
  llvm::DenseMap<const FunctionDecl *, std::vector<const CallExpr *>> callSites;
  llvm::DenseMap<const FunctionDecl *, std::vector<const FunctionDecl *>> callees;

private:
  static std::string calleeName(const CallExpr *c);
  static void childrenOf(const Expr *e, const std::function<void(const Expr *)> &visit);

  void walkProgram();
  void expr(const Expr *e);
  void stmt(const Stmt *s);
  void call(const CallExpr *c);
  void create(const NewExpr *n);
  void topLevel(const Stmt *s);
  void assign(const AssignExpr *a);
  void function(const FunctionDecl *fn);
  void keep(const Expr *value, llvm::StringRef where);
  void passed(const CallExpr *c, const FunctionDecl *target, llvm::StringRef name);

  bool reporting;
  llvm::DenseSet<const char *> reported;
  llvm::DenseSet<const FunctionDecl *> walked;
  llvm::DenseSet<const FunctionDecl *> heldThis;
  std::set<std::pair<const FunctionDecl *, unsigned>> heldParams;
  std::vector<std::pair<const char *, const char *>> libraryRanges;
};

}
