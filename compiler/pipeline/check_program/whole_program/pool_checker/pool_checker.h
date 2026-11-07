#pragma once

#include "compiler/pipeline/check_program/whole_program/held_value_checker/held_value_checker.h"

namespace tz {

class PoolChecker : public HeldValueChecker {
public:
  explicit PoolChecker(Program &checked);

private:
  using Site = std::pair<const CallExpr *, const FunctionDecl *>;

  static bool poolClass(const ClassDecl *cls);

  bool used() const override;
  void kept(const Expr *value, bool closure, llvm::StringRef where) override;
  bool produces(const CallExpr *c) const override;
  bool takesOver(const CallExpr *c) override;
  void sawNew(const NewExpr *n) override;
  void sawVoid(const CallExpr *c) override;
  void sawCall(const CallExpr *c) override;
  bool heldMember(const MemberExpr *m) const override;
  void spreadKept(const Expr *value) override;
  size_t extraKnown() const override;
  bool closuresCarry() const override;
  void finish() override;
  void startWalk() override;

  void checkStoreRuns();
  bool storeValue(const Expr *value) const;
  const FunctionDecl *storeReader(const FunctionDecl *fn) const;
  void runRoots(const Expr *callback, unsigned depth, std::vector<const FunctionDecl *> &roots);

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
