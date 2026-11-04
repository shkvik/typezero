#pragma once

#include "compiler/library/held_values/held_value_checker/held_value_checker.h"

namespace tz {

class ContextChecker : public HeldValueChecker {
public:
  explicit ContextChecker(Program &program);

private:
  bool used() const override;
  bool produces(const CallExpr *c) const override;
  bool takesOver(const CallExpr *c) override;
  void sawNew(const NewExpr *n) override;
  void kept(const Expr *value, bool closure, const llvm::Twine &where) override;
  void finish() override;
  void startWalk() override;

  void checkNews();
  void checkRuns();
  void checkIntervals();
  const FunctionDecl *reader(const FunctionDecl *fn) const;

  llvm::DenseSet<const FunctionDecl *> readsDirectly_;
  std::vector<const CallExpr *> intervals_;
  std::vector<const CallExpr *> runs_;
  std::vector<const NewExpr *> news_;
};
}
