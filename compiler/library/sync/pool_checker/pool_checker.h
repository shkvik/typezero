#pragma once

#include "compiler/library/held_values/held_value_checker/held_value_checker.h"

namespace tz {

class PoolChecker : public HeldValueChecker {
public:
  explicit PoolChecker(Program &program);

private:
  bool used() const override;
  bool takesOver(const CallExpr *c) override;
  void sawNew(const NewExpr *n) override;
  void sawVoid(const CallExpr *c) override;
  bool closuresCarry() const override;
  void kept(const Expr *value, bool closure, const llvm::Twine &where) override;
  void finish() override;
  void startWalk() override;

  static bool poolClass(const ClassDecl *cls);

  std::vector<std::pair<const Expr *, const FunctionDecl *>> uses_;
};

}
