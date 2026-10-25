#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

// Loops: the counters, the passes until the ranges stop growing.
class LoopRanges {
public:
  explicit LoopRanges(OverflowCheck &check);

  void execLoop(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels, State &st);
  State onePass(const OverflowCheck::LoopSpec &spec, const State &head, State &exits);
  void runLoop(const OverflowCheck::LoopSpec &spec, State &st);
  // `from` and `end`: a pass from `head`, each value relative to itself.
  Num passCount(const OverflowCheck::LoopSpec &spec, const State &entry, const State &head, const State &from,
                const State &end);
  State rebased(const State &state, llvm::DenseMap<const VarSymbol *, uint64_t> &bases);

private:
  OverflowCheck &check_;
};

} // namespace tz
