#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/program/program.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Twine.h"
#include <set>
#include <utility>
#include <vector>

namespace tz {

class HeldValueChecker {
public:
  explicit HeldValueChecker(Program &program);
  virtual ~HeldValueChecker() = default;

  void run();

protected:
  virtual bool used() const = 0;
  virtual bool produces(const CallExpr *c) const;
  virtual bool takesOver(const CallExpr *c);
  virtual void sawNew(const NewExpr *n);
  virtual void sawVoid(const CallExpr *c);
  virtual void sawCall(const CallExpr *c);
  virtual bool heldMember(const MemberExpr *m) const;
  virtual void spreadKept(const Expr *value);
  virtual size_t extraKnown() const;
  virtual bool closuresCarry() const;
  virtual void kept(const Expr *value, bool closure, const llvm::Twine &where) = 0;
  virtual void finish();
  virtual void startWalk();

  bool usesModule(llvm::StringRef name) const;
  bool inLibrary(llvm::SMLoc loc) const;
  bool isHeld(const Expr *e) const;
  bool carries(const FunctionDecl *fn) const;
  void markParam(const FunctionDecl *fn, unsigned index);
  void markThis(const FunctionDecl *fn);
  bool once(const Expr *value);
  const FunctionDecl *programTarget(const CallExpr *c) const;
  static const Expr *bare(const Expr *e);
  static const FunctionDecl *callbackOf(const Expr *e);
  static const ClassDecl *storageClass(const ClassDecl *cls);
  static bool storageMethod(const CallExpr *c, llvm::StringRef name);
  static const VarSymbol *storageOf(const CallExpr *c);

  Program &program_;
  const FunctionDecl *current_ = nullptr;
  bool report_ = false;
  llvm::DenseSet<const VarSymbol *> heldVars_;
  llvm::DenseSet<const FunctionDecl *> returnsHeld_;
  llvm::DenseMap<const FunctionDecl *, std::vector<const FunctionDecl *>> callees_;
  llvm::DenseSet<const Expr *> allowedNews_;
  llvm::DenseSet<const Expr *> awaited_;
  llvm::DenseSet<const Expr *> allowedStores_;
  llvm::DenseMap<const FunctionDecl *, std::vector<const CallExpr *>> callSites_;

private:
  void walkProgram();
  void function(const FunctionDecl *fn);
  void topLevel(const Stmt *s);
  void stmt(const Stmt *s);
  void expr(const Expr *e);
  void call(const CallExpr *c);
  void create(const NewExpr *n);
  void assign(const AssignExpr *a);
  void passed(const CallExpr *c, const FunctionDecl *target, llvm::StringRef name);
  void keep(const Expr *value, const llvm::Twine &where);

  std::vector<std::pair<const char *, const char *>> libraryRanges_;
  std::set<std::pair<const FunctionDecl *, unsigned>> heldParams_;
  llvm::DenseSet<const FunctionDecl *> heldThis_;
  llvm::DenseSet<const FunctionDecl *> walked_;
  llvm::DenseSet<const char *> reported_;
};

}
