#pragma once

#include "compiler/library/regex/matcher_builder/matcher_builder.h"

namespace tz {

// The memo of states already tried, and the budget of work.
class MatcherMemo {
public:
  explicit MatcherMemo(MatcherBuilder &builder);

  // The memo
  llvm::Value *memoContext(uint32_t pc, llvm::Value *pos);
  void memoCheck(uint32_t row, uint32_t pc, llvm::Value *pos, llvm::BasicBlock *visited);
  void budgetCheck();
  void countWork(llvm::Value *from, llvm::Value *to);
  llvm::StructType *memoType();
  llvm::Value *memoLogLength();
  void withMemo(llvm::StringRef name, llvm::ArrayRef<llvm::Value *> args);

private:
  MatcherBuilder &builder_;
};

} // namespace tz
