#pragma once

#include "compiler/library/regex/matcher_builder/matcher_builder.h"

namespace tz {

class MatcherAnalysis {
public:
  explicit MatcherAnalysis(MatcherBuilder &builder);

  void analyze();
  bool inLoop(const CountedLoop &loop, uint32_t pc) const;
  bool flagged(const CountedLoop &loop, uint32_t pc) const;
  uint64_t contextSize(uint32_t pc) const;
  void planMemo();
  void planScan();
  std::vector<OffsetSet> offsetSets(unsigned limit) const;
  void closure(std::set<uint32_t> &states, bool &match, bool &unknown) const;

private:
  MatcherBuilder &builder_;
};

}
