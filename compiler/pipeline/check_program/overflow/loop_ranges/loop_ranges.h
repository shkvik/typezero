#pragma once

#include "compiler/pipeline/check_program/overflow/overflow_check/overflow_check.h"

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
  void reportLong();
  void findRecursions();
  void noteCall(const CallExpr *call, const FunctionDecl *target, llvm::ArrayRef<Val> args);

private:
  static bool yields(const Stmt *stmt);
  static bool openEnded(const Expr *cond, const Stmt *body);
  static bool grows(const Stmt *body, const VarSymbol *var);
  static bool identicalNum(const Num &a, const Num &b);
  static bool identicalVal(const Val &a, const Val &b);
  static bool identicalState(const State &a, const State &b);
  static bool awaitsIn(const Stmt *stmt);
  static bool waitsOrYields(const Stmt *stmt);
  static bool readsInput(const Expr *cond);
  static std::optional<double> counterStep(const Expr *arg, const VarSymbol *param);
  static bool limitedTowards(const Num &n, double step);
  static bool freshBase(const Val &v, uint64_t before);
  static std::optional<std::pair<double, double>> stepOf(const Num &end, uint64_t base);
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
  void reportRecursions();
  std::vector<uint64_t> contextOf() const;
  bool freshBases(const State &state, uint64_t before) const;

  OverflowCheck &check_;
  llvm::DenseMap<const char *, std::vector<RememberedLoop>> remembered_;
};

}
