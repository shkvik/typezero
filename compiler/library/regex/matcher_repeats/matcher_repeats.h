#pragma once

#include "compiler/library/regex/matcher_builder/matcher_builder.h"

namespace tz {

// Greedy and lazy repeats.
class MatcherRepeats {
public:
  explicit MatcherRepeats(MatcherBuilder &builder);

  void emitRepeatGreedy(uint32_t pc, bool memo);
  void emitRepeatLazy(uint32_t pc);
  uint32_t greedyLabel(uint32_t pc);
  uint32_t lazyLabel(uint32_t pc);
  llvm::Value *unitMatches(uint32_t atom, llvm::Value *u);
  FirstUnits unitsFirst(uint32_t pc) const;
  llvm::Value *startsWith(const FirstUnits &units, llvm::Value *pos);

private:
  MatcherBuilder &builder_;
};

} // namespace tz
