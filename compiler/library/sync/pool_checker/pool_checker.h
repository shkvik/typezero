#pragma once

#include "compiler/library/held_values/held_value_checker/held_value_checker.h"

namespace tz {

class PoolChecker : public HeldValueChecker {
public:
  explicit PoolChecker(Program &program);

private:
  bool used() const override;
  bool produces(const CallExpr *c) const override;
  bool takesOver(const CallExpr *c) override;
  void sawNew(const NewExpr *n) override;
  void sawVoid(const CallExpr *c) override;
  void sawCall(const CallExpr *c) override;
  bool heldMember(const MemberExpr *m) const override;
  void spreadKept(const Expr *value) override;
  size_t extraKnown() const override;
  bool closuresCarry() const override;
  void kept(const Expr *value, bool closure, const llvm::Twine &where) override;
  void finish() override;
  void startWalk() override;

  void checkStoreRuns();
  bool storeValue(const Expr *value) const;
  void runRoots(const Expr *callback, unsigned depth, std::vector<const FunctionDecl *> &roots);
  const FunctionDecl *storeReader(const FunctionDecl *fn) const;

  static bool poolClass(const ClassDecl *cls);

  using Site = std::pair<const CallExpr *, const FunctionDecl *>;

  std::vector<std::pair<const Expr *, const FunctionDecl *>> uses_;
  llvm::DenseSet<const VarSymbol *> poolStores_;
  llvm::DenseSet<const Type *> itemTypes_;
  llvm::DenseSet<const Type *> storeTypes_;
  llvm::DenseSet<const FunctionDecl *> readsStore_;
  std::vector<Site> runs_;
  std::vector<Site> voids_;
  std::vector<Site> timers_;
};

}
