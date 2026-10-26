#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ReflectArgsLowerer {
public:
  explicit ReflectArgsLowerer(Lowering &lowering);

  llvm::Value *emitChooseVariant(llvm::ArrayRef<const FunctionDecl *> variants, llvm::Value *selfId,
                                 llvm::Value *args, llvm::Value *count);
  void emitReflectArgs(llvm::Value *args, llvm::Value *count, size_t min,
                       llvm::ArrayRef<const Type *> params, const Type *rest,
                       std::vector<llvm::Value *> &values,
                       llvm::function_ref<void(llvm::Value *, const Type *, llvm::Value *)> onFail);
  void emitReflectMismatch(llvm::StringRef what, llvm::Value *index, const Type *expected,
                           llvm::Value *got);
  void emitReflectFail(llvm::StringRef code, const std::string &message);

private:
  Lowering &lowering_;
};

}
