#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ThrowLowerer {
public:
  explicit ThrowLowerer(Lowering &lowering);

  llvm::GlobalVariable *thrownGlobal();
  llvm::Value *thrownSlot();
  llvm::Value *thrownSlot(llvm::IRBuilderBase &b);
  void emitThrowCheck();
  void emitThrowCheckWith(llvm::function_ref<void()> release);
  void emitThrow(llvm::Value *error, llvm::SMLoc loc);
  void emitThrowNew(int kind, llvm::StringRef message);
  void emitRuntimeThrow();
  void emitThrowStmt(ThrowStmt *stmt);
  void emitStackCheck(llvm::SMLoc loc);
  Lowering::Held hold(llvm::Value *value, const Type *type, bool needed);
  llvm::Value *take(Lowering::Held &held);
  void holdOperand(Lowering::Operand &operand, bool needed);
  void takeOperand(Lowering::Operand &operand);
  std::vector<llvm::Value *> emitHeldValues(llvm::ArrayRef<Expr *> exprs);

private:
  Lowering &lowering_;
  llvm::GlobalVariable *thrown_ = nullptr;
};

}
