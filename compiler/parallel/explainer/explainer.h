#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class Explainer {
public:
  explicit Explainer(Analysis &analysis);

  void print(llvm::raw_ostream &out);
  std::vector<std::string> decisionLines(unsigned piece) const;

private:
  Analysis &analysis_;
};

}
