#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class UnwindLowerer {
public:
  explicit UnwindLowerer(Lowering &lowering);

  llvm::Constant *frameDescriptor(llvm::SMLoc loc, const std::string *function = nullptr);
  std::string frameFunctionName() const;
  llvm::BasicBlock *unwindTarget();
  llvm::BasicBlock *unwindBlock(size_t scope);
  llvm::BasicBlock *outerUnwind(size_t scope);
  void finishScopeUnwind(size_t i);
  void finishFunctionUnwind();

private:
  Lowering &lowering_;
  std::map<std::pair<std::string, const char *>, llvm::Constant *> frames_;
  llvm::StringMap<std::string> filePaths_;
};

}
