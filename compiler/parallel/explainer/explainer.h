#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// tz build --explain-parallel: the decisions in words.
class Explainer {
public:
  explicit Explainer(Analysis &analysis);

  void print(llvm::raw_ostream &out);
  std::vector<std::string> decisionLines(unsigned piece) const;

private:
  Analysis &analysis_;
};

} // namespace tz::parallel
