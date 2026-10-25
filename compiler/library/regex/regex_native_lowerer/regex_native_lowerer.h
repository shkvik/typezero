#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The machine code of regex literals.
class RegexNativeLowerer {
public:
  explicit RegexNativeLowerer(Lowering &lowering);

  // The machine code of a literal's program, a function of the type
  // TzReMatch (codegen_regex_native.cpp).
  llvm::Constant *regexMatcher(const RegexLitExpr *lit);

private:
  Lowering &lowering_;
  std::map<std::vector<uint32_t>, llvm::Constant *> regexMatchers_;
};

} // namespace tz
