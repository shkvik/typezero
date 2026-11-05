#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <vector>

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
