#pragma once

#include "compiler/library/regex/matcher_builder/matcher_builder.h"

namespace tz {

// The head of the matcher: the width of units and the scan for where a match starts.
class MatcherHead {
public:
  explicit MatcherHead(MatcherBuilder &builder);

  // Emitting
  void emitWidth(bool wide);
  void emitHead();
  void emitByteScan(llvm::Value *from, llvm::BasicBlock *found, llvm::AllocaInst *at,
                    llvm::BasicBlock *none);
  llvm::Value *vectorIn(llvm::Value *v, const OffsetSet &units);
  llvm::Value *unitMask(llvm::Value *at, const OffsetSet &units);

private:
  MatcherBuilder &builder_;
};

} // namespace tz
