#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// throw, the check after a call that may throw, and the check of the stack.
class ThrowLowerer {
public:
  explicit ThrowLowerer(CodeGen &codegen);

  // A stack slot for the error in flight and the stack limit, the runtime's.
  llvm::GlobalVariable *thrownGlobal();
  // The address of this thread's tz_thrown, made by builder_ or by `b`.
  llvm::Value *thrownSlot();
  llvm::Value *thrownSlot(llvm::IRBuilderBase &b);
  // After a call that may throw (at loc_): an error in flight goes up.
  void emitThrowCheck();
  // emitThrowCheck, with `release` on the way up for what only the code at
  // hand knows of.
  void emitThrowCheckWith(llvm::function_ref<void()> release);
  // Throws `error`, an owned Error, from `loc`; the block ends.
  void emitThrow(llvm::Value *error, llvm::SMLoc loc);
  // A built-in fails at loc_: throws a new Error of `kind` (an index of
  // kErrorKinds) with `message`. The block ends.
  void emitThrowNew(int kind, llvm::StringRef message);
  // An error the runtime made (a function of it that may throw was called):
  // goes up from loc_. The block ends.
  void emitRuntimeThrow();
  void emitThrowStmt(ThrowStmt *stmt);
  // At the start of a function that may recurse: RangeError if the stack is
  // too low.
  void emitStackCheck(llvm::SMLoc loc);
  CodeGen::Held hold(llvm::Value *value, const Type *type, bool needed);
  llvm::Value *take(CodeGen::Held &held);
  void holdOperand(CodeGen::Operand &operand, bool needed);
  void takeOperand(CodeGen::Operand &operand);
  // Evaluates `exprs` in order, owned; each managed one is held while one
  // after it that may throw runs.
  std::vector<llvm::Value *> emitHeldValues(llvm::ArrayRef<Expr *> exprs);

private:
  CodeGen &codegen_;
  llvm::GlobalVariable *thrown_ = nullptr; // tz_thread
};

} // namespace tz
