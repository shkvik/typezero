#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The way of an error up: the frames of functions and the cleanups on the way.
class UnwindLowerer {
public:
  explicit UnwindLowerer(CodeGen &codegen);

  // A constant TzFrame for `loc` in the function being emitted, or named
  // `function`.
  llvm::Constant *frameDescriptor(llvm::SMLoc loc, const std::string *function = nullptr);
  std::string frameFunctionName() const;
  // Where an error at the current point goes: the release of the innermost
  // scope, then outward.
  llvm::BasicBlock *unwindTarget();
  llvm::BasicBlock *unwindBlock(size_t scope);
  llvm::BasicBlock *outerUnwind(size_t scope);
  // Fills the unwind block of scope `i`, which is ending.
  void finishScopeUnwind(size_t i);
  // Fills the block where an error leaves the function being emitted.
  void finishFunctionUnwind();

private:
  CodeGen &codegen_;
  std::map<std::pair<std::string, const char *>, llvm::Constant *> frames_;
  llvm::StringMap<std::string> filePaths_;
};

} // namespace tz
