#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// Which pieces may not run at the same time, and what runs in parallel.
class Conflicts {
public:
  explicit Conflicts(Analysis &analysis);

  bool concurrent(unsigned a, unsigned b) const;
  uint64_t maskOf(const Loc &loc) const;
  void decide();
  std::vector<Touch> touches(const Effects &effects) const;

private:
  Analysis &analysis_;
};

} // namespace tz::parallel
