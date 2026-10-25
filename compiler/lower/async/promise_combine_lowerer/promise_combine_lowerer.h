#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Promise.all, allSettled, race and any.
class PromiseCombineLowerer {
public:
  explicit PromiseCombineLowerer(CodeGen &codegen);

  llvm::Value *emitPromiseCombine(CallExpr *call);
  // void (ptr dst, ptr src): the value of type `from` at src, made a `to`
  // (a union of it), into dst; race and any of promises of different values.
  llvm::Function *combineConverter(const Type *from, const Type *to);

private:
  CodeGen &codegen_;
  std::map<std::pair<const Type *, const Type *>, llvm::Function *> combineConverters_;
};

} // namespace tz
