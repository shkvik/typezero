#pragma once

#include "compiler/library/regex/matcher_builder/matcher_builder.h"

namespace tz {

class MatcherInstructions {
public:
  explicit MatcherInstructions(MatcherBuilder &builder);

  void emitInstruction(uint32_t pc);
  bool literalUnit(uint32_t pc, uint32_t &unit, uint32_t &fold) const;
  uint32_t literalRun(uint32_t pc) const;
  uint32_t emitLiteralRun(uint32_t pc, uint32_t run);
  void emitRepeat(uint32_t pc);
  void emitLook(uint32_t pc);
  void emitLookSuccess(uint32_t look);

private:
  MatcherBuilder &builder_;
};

}
