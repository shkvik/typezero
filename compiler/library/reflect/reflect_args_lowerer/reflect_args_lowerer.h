#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The arguments of Reflect.apply and construct, and the variant they choose.
class ReflectArgsLowerer {
public:
  explicit ReflectArgsLowerer(CodeGen &codegen);

  // The position among `variants` of the one the arguments `args[0..count)`
  // (unknowns, borrowed) fit best, as a call that compiles chooses: each
  // argument fits it no worse than any other variant that takes them, and one
  // better; -1 if none takes them, -2 if several fit equally well. A method
  // takes only a `this` of its class, whose number `selfId` gives (null for
  // constructors).
  llvm::Value *emitChooseVariant(llvm::ArrayRef<const FunctionDecl *> variants, llvm::Value *selfId,
                                 llvm::Value *args, llvm::Value *count);
  // The arguments `args[0..count)` (unknowns, borrowed) for the parameters
  // `params` and the rest parameter `rest` (an array type, or null), appended
  // to `values`; `min` of them are surely there. On a mismatch, what was
  // converted is released and `onFail` gets the parameter, its type and the
  // argument, and must leave the block.
  void emitReflectArgs(llvm::Value *args, llvm::Value *count, size_t min,
                       llvm::ArrayRef<const Type *> params, const Type *rest,
                       std::vector<llvm::Value *> &values,
                       llvm::function_ref<void(llvm::Value *, const Type *, llvm::Value *)> onFail);
  // The TypeError of an argument `got` (borrowed) that is no `expected`.
  void emitReflectMismatch(llvm::StringRef what, llvm::Value *index, const Type *expected,
                           llvm::Value *got);

private:
  CodeGen &codegen_;
};

} // namespace tz
