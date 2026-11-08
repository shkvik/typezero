#pragma once

#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

class RegexNativeLowerer {
public:
  explicit RegexNativeLowerer(Lowering &owner);

  llvm::Constant *regexMatcher(const RegexLitExpr *lit);

private:
  static bool isBack(uint32_t op);
  static bool isCharInstruction(uint32_t op);
  static bool hasByte(const ByteSet &s, uint32_t c);
  static void addByte(ByteSet &s, uint32_t c);
  static uint64_t counterRange(const CountedLoop &loop);

  Lowering &lowering;
  std::map<std::vector<uint32_t>, llvm::Constant *> regexMatchers;
};

}
