#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

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

}
