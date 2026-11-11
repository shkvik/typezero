#pragma once

#include "compiler/interprocedural/held_value_checker/held_value_checker.h"

namespace tz {

class PoolChecker : public HeldValueChecker {
public:
  PoolChecker(Program &checked, const LibraryRanges &library);

private:
  using Site = std::pair<const CallExpr *, const FunctionDecl *>;

  struct PassedCallback {
    const Expr *callback;
    unsigned depth;
  };

  static bool poolClass(const ClassDecl *cls);

  bool used() const override;
  std::vector<Diagnostic> kept(const Expr *value, bool closure, llvm::StringRef where) override;
  bool produces(const CallExpr *c) const override;
  bool takesOver(const CallExpr *c) override;
  void sawNew(const NewExpr *n) override;
  std::vector<Diagnostic> sawVoid(const CallExpr *c) override;
  std::vector<Diagnostic> sawCall(const CallExpr *c) override;
  bool heldWithObject(const MemberExpr *m) const override;
  std::vector<Diagnostic> spreadKept(const Expr *value) override;
  size_t extraKnown() const override;
  bool closuresCarry() const override;
  std::vector<Diagnostic> finish() override;
  void startWalk() override;

  std::vector<Diagnostic> findLateStoreReads();
  std::vector<Diagnostic> findReturnedItems();
  bool storeValue(const Expr *value) const;
  const FunctionDecl *storeReader(const FunctionDecl *fn) const;
  std::vector<const FunctionDecl *> runRoots(const Expr *runCallback) const;
  std::vector<const Expr *> argsAtCallSites(const VarSymbol *param) const;

  std::vector<Site> runs;
  std::vector<Site> voids;
  std::vector<Site> timers;
  llvm::DenseSet<const Type *> itemTypes;
  llvm::DenseSet<const Type *> storeTypes;
  llvm::DenseSet<const VarSymbol *> poolStores;
  llvm::DenseSet<const FunctionDecl *> readsStore;
  std::vector<std::pair<const Expr *, const FunctionDecl *>> uses;
};

}
