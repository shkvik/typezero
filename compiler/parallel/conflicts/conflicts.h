#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/ast/ast.h"
#include <cstdint>
#include <vector>

namespace tz::parallel {

class Conflicts {
public:
  explicit Conflicts(Analysis &analysis);

  bool concurrent(unsigned a, unsigned b) const;
  uint64_t maskOf(const Loc &loc) const;
  void decide();
  std::vector<Touch> touches(const Effects &effects) const;

private:
  void placeWaits();
  bool reachers(const FunctionDecl *home, std::vector<unsigned> &out) const;
  Analysis &analysis_;
};

}
