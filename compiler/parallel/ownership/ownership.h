#pragma once

#include "compiler/parallel/analysis/analysis.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace tz::parallel {

class Ownership {
public:
  explicit Ownership(Analysis &analysis);

  std::optional<Ref> ownedRef(const Expr *e);
  void decide();
  Ref substituted(Ref r, const FnInfo &callee, const FnInfo &caller, const CallRec &rec, Mode mode);
  void rewrite();

private:
  using Roots = llvm::SmallVector<const VarSymbol *, 2>;
  struct Site {
    unsigned fn = 0;
    unsigned rec = 0;
  };
  const VarSymbol *rootOf(const FunctionDecl *fn, const Expr *e, unsigned depth) const;
  static bool isParam(const FunctionDecl *fn, const VarSymbol *v);
  bool candidate(const FunctionDecl *fn, const VarSymbol *v) const;
  static bool tracked(const FunctionDecl *fn, const VarSymbol *v);
  static bool libraryField(const Type *object, llvm::StringRef name);
  static bool plainValues(const Type *type, unsigned depth);
  static bool derivable(const Type *type, unsigned depth);
  static bool valueArgument(const Expr *e);
  void carriers(const Expr *e, Roots &out, unsigned depth) const;
  void leak(const Expr *e);
  void visitStmt(const Stmt *s);
  void visit(const Expr *e);
  void visitCall(const CallExpr *c);
  bool passesOwned(const CallExpr *c, size_t index) const;
  bool callersOwned(unsigned fi, size_t index) const;
  Ref resolved(Ref r) const;
  Ref interned(const VarSymbol *root, const Type *type);
  void scan(unsigned fi);
  Analysis &analysis_;
  const FunctionDecl *fn_ = nullptr;
  unsigned fi_ = 0;
  llvm::DenseSet<const VarSymbol *> leaked_;
  llvm::DenseSet<const VarSymbol *> leaking_;
  llvm::DenseMap<const VarSymbol *, std::pair<const FunctionDecl *, unsigned>> paramOf_;
  llvm::DenseMap<const VarSymbol *, const Expr *> loopInit_;
  std::vector<std::vector<Site>> sites_;
};

}
