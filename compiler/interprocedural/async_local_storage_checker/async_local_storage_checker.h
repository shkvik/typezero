#pragma once

#include "compiler/interprocedural/held_value_checker/held_value_checker.h"

namespace tz {

class AsyncLocalStorageChecker : public HeldValueChecker {
public:
  AsyncLocalStorageChecker(Program &checked, const LibraryRanges &library);

private:
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

  std::vector<Diagnostic> findMisplacedNews() const;
  std::vector<Diagnostic> findReusedStores() const;
  std::vector<Diagnostic> findStoreReadingTimers() const;
  const FunctionDecl *reader(const FunctionDecl *fn) const;

  std::vector<const NewExpr *> news;
  std::vector<const CallExpr *> runs;
  std::vector<const CallExpr *> intervals;
  llvm::DenseSet<const FunctionDecl *> readsDirectly;
};

}
