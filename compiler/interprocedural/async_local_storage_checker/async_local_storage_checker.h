#pragma once

#include "compiler/interprocedural/held_value_checker/held_value_checker.h"

namespace tz {

class AsyncLocalStorageChecker : public HeldValueChecker {
public:
  AsyncLocalStorageChecker(Program &checked, const LibraryRanges &library);

private:
  bool used() const override;
  void kept(const Expr *value, bool closure, llvm::StringRef where) override;
  bool produces(const CallExpr *c) const override;
  bool takesOver(const CallExpr *c) override;
  void sawNew(const NewExpr *n) override;
  void finish() override;
  void startWalk() override;

  void checkNews();
  void checkRuns();
  void checkIntervals();
  const FunctionDecl *reader(const FunctionDecl *fn) const;

  std::vector<const NewExpr *> news;
  std::vector<const CallExpr *> runs;
  std::vector<const CallExpr *> intervals;
  llvm::DenseSet<const FunctionDecl *> readsDirectly;
};

}
