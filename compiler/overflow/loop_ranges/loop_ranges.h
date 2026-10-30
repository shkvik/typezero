#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

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

private:
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
  std::vector<uint64_t> contextOf() const;
  bool freshBases(const State &state, uint64_t before) const;

  OverflowCheck &check_;
  llvm::DenseMap<const char *, std::vector<RememberedLoop>> remembered_;
};

}
