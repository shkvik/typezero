#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class RegexNativeLowerer {
public:
  explicit RegexNativeLowerer(Lowering &lowering);

  llvm::Constant *regexMatcher(const RegexLitExpr *lit);

private:
  Lowering &lowering_;
  std::map<std::vector<uint32_t>, llvm::Constant *> regexMatchers_;
};

}
