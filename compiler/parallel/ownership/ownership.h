#pragma once

#include "compiler/parallel/analysis/analysis.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace tz::parallel {

class Ownership {
public:
  explicit Ownership(Analysis &analysis);

  static bool valueArgument(const Expr *e);
  std::optional<Ref> ownedRef(const Expr *e);
  void decide();
  std::optional<Ref> substituted(Ref r, const FnInfo &callee, unsigned caller, const CallRec &rec,
                                 const Callee &call);
  void rewrite();

private:
  using Roots = llvm::SmallVector<const VarSymbol *, 2>;
  using Needs = std::vector<FieldKey>;
  struct Site {
    unsigned fn = 0;
    unsigned rec = 0;
  };
  struct Store {
    const Type *type = nullptr;
    const Expr *value = nullptr;
    unsigned fn = 0;
    const VarSymbol *holder = nullptr;
  };
  const VarSymbol *rootOf(const FunctionDecl *fn, const Expr *e, Needs &needs, unsigned depth);
  static bool isParam(const FunctionDecl *fn, const VarSymbol *v);
  bool candidate(const FunctionDecl *fn, const VarSymbol *v) const;
  static bool tracked(const FunctionDecl *fn, const VarSymbol *v);
  bool freshCandidate(const FunctionDecl *fn, const VarSymbol *v) const;
  bool freshAllocation(const Expr *e) const;
  const CallExpr *programCall(const Expr *e) const;
  static bool libraryField(const Type *object, llvm::StringRef name);
  static bool plainObject(const Type *type);
  bool container(const Type *type);
  void libraryTypes(const Stmt *item, std::set<const Type *> &out);
  static bool plainValues(const Type *type, unsigned depth);
  static bool derivable(const Type *type, unsigned depth);
  void carriers(const Expr *e, Roots &out, unsigned depth);
  void leak(const Expr *e);
  void leakParams(const Expr *e);
  void visitStmt(const Stmt *s);
  void visit(const Expr *e);
  void visitCall(const CallExpr *c);
  void literal(const ObjectLitExpr *o);
  void dirty(const Expr *object, llvm::StringRef field);
  void dirtyAll(const Expr *e);
  bool thisFirst(const CallRec &rec) const;
  bool tailAwaited(unsigned fi);
  bool handsOver(unsigned fi, const CallRec &rec, Mode mode, bool strict);
  int argOf(unsigned fi, const CallRec &rec, const Callee &callee, size_t param, bool strict);
  bool passesOwned(const CallExpr *c, size_t index);
  bool callersOwned(unsigned fi, size_t index, bool strict);
  bool callFresh(unsigned fi, const CallExpr *c) const;
  bool valueOwn(unsigned fi, const Expr *e, bool params, unsigned depth);
  bool fieldOpen(const FieldKey &key);
  bool fieldOwn(const FieldKey &key);
  bool needsOwn(const Needs &needs);
  bool own(const OwnedRef *owned);
  Ref interned(const VarSymbol *root, const Type *type, Needs needs, bool whole);
  void scan(unsigned fi);
  Analysis &analysis_;
  const FunctionDecl *fn_ = nullptr;
  unsigned fi_ = 0;
  bool collecting_ = false;
  const Expr *heldLiteral_ = nullptr;
  const VarSymbol *holder_ = nullptr;
  llvm::DenseSet<const VarSymbol *> leaked_;
  llvm::DenseSet<const VarSymbol *> leaking_;
  llvm::DenseSet<const VarSymbol *> owned_;
  llvm::DenseMap<const VarSymbol *, std::pair<const FunctionDecl *, unsigned>> paramOf_;
  llvm::DenseMap<const VarSymbol *, unsigned> freshOf_;
  llvm::DenseMap<const VarSymbol *, const Expr *> loopInit_;
  std::vector<std::vector<Site>> sites_;
  std::vector<std::vector<const Expr *>> returns_;
  std::map<const char *, std::vector<Store>> stores_;
  std::map<const char *, std::vector<const Type *>> dirty_;
  std::map<FieldKey, bool> open_;
  std::set<FieldKey> asked_;
  std::set<FieldKey> evaluated_;
  std::set<FieldKey> notOwn_;
  bool deciding_ = false;
  llvm::DenseSet<unsigned> fresh_;
  std::vector<int8_t> tail_;
  std::optional<llvm::DenseSet<const Type *>> containers_;
};

}
