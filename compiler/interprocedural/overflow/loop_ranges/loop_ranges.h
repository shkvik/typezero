#pragma once

#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"

namespace tz {

class LoopRanges {
public:
  explicit LoopRanges(OverflowCheck &check);

  void execLoop(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels, State &st);
  State onePass(const OverflowCheck::LoopSpec &spec, const State &head, State &exits);
  void runLoop(const OverflowCheck::LoopSpec &spec, State &st);
  Num passCount(const OverflowCheck::LoopSpec &spec, const State &entry, const State &head,
                const State &from, const State &end);
  State rebased(const State &state, llvm::DenseMap<const VarSymbol *, uint64_t> &bases);
  void forget();
  std::vector<Diagnostic> findLongLoops();
  void findRecursions();
  void noteCall(const CallExpr *call, const FunctionDecl *target, llvm::ArrayRef<Val> args);

private:
  bool yields(const Stmt *stmt) const;
  bool openEnded(const Expr *cond, const Stmt *body) const;
  bool grows(const Stmt *body, const VarSymbol *var) const;
  bool identicalNum(const Num &a, const Num &b) const;
  bool identicalVal(const Val &a, const Val &b) const;
  bool identicalState(const State &a, const State &b) const;
  bool awaitsIn(const Stmt *stmt) const;
  std::vector<const Stmt *> awaitParts(const Stmt *stmt) const;
  bool waitsOrYields(const Stmt *stmt) const;
  bool readsInput(const Expr *cond) const;
  std::optional<double> counterStep(const Expr *arg, const VarSymbol *param) const;
  bool limitedTowards(const Num &n, double step) const;
  bool freshBase(const Val &v, uint64_t before) const;
  std::optional<std::pair<double, double>> stepOf(const Num &end, uint64_t base) const;
  struct RememberedLoop {
    std::vector<uint64_t> context;
    State entry;
    std::optional<Val> source;
    State exit;
    std::vector<std::pair<State, State>> jumps;
    State returned;
    Val result;
    uint64_t work = 0;
  };
  void solveLoop(const OverflowCheck::LoopSpec &spec, State &st);
  void noteLoop(const OverflowCheck::LoopSpec &spec, const Num &passes);
  std::vector<Diagnostic> findLongRecursions();
  std::vector<uint64_t> contextOf() const;
  bool freshBases(const State &state, uint64_t before) const;

  OverflowCheck &check;
  llvm::DenseMap<const char *, std::vector<RememberedLoop>> remembered;
};

}
