#pragma once

#include "compiler/library/regex/matcher_builder/matcher_builder.h"

namespace tz {

// Characters: reading them and testing them against sets.
class MatcherChars {
public:
  explicit MatcherChars(MatcherBuilder &builder);

  // A character at a position: the code point and the position past it (or
  // before it, back), or a jump to `fail`.
  std::pair<llvm::Value *, llvm::Value *> readChar(llvm::Value *pos, bool back,
                                                   llvm::BasicBlock *fail);
  llvm::Value *charStep(uint32_t atom, llvm::Value *pos, llvm::BasicBlock *fail);
  llvm::Value *inSet(llvm::Value *c, const uint32_t *set, bool bytesOnly);
  llvm::Value *inByteSet(llvm::Value *c, const ByteSet &bits);
  llvm::Constant *byteTable(const ByteSet &bits);
  llvm::Value *isLineTerminator(llvm::Value *c);
  llvm::Value *isWordUnit(llvm::Value *u, bool folded);
  llvm::Value *isLead(llvm::Value *u);
  llvm::Value *isTrail(llvm::Value *u);
  llvm::Value *test(llvm::Value *cond, const std::function<llvm::Value *()> &value);

private:
  MatcherBuilder &builder_;
};

} // namespace tz
