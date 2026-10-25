#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Stores of references: into groups, across threads, and the drops of members.
class StoreLowerer {
public:
  explicit StoreLowerer(CodeGen &codegen);

  // After `owner` took over the owned `value` of `type` into a slot of its:
  // joins the groups of what it refers to (tz_group_store).
  void groupStore(llvm::Value *owner, llvm::Value *value, const Type *type);
  // The parallel runtime (runtime/core/publish/publish.c): before a value goes
  // where other threads may reach it, it becomes shared. storeBarrier: into
  // `owner`, an object of the heap, which is shared or not; publishValue: into
  // a global that functions see.
  void storeBarrier(llvm::Value *owner, llvm::Value *value, const Type *type);
  void publishValue(llvm::Value *value, const Type *type);
  // Releases `value`, which a slot of `owner` held.
  void releaseIn(llvm::Value *owner, llvm::Value *value, const Type *type);
  // `value` moved out of a slot of `owner` and is owned by the code now.
  void takeFrom(llvm::Value *owner, llvm::Value *value, const Type *type);
  // Stores an owned value into a slot of `owner` (of static type
  // `ownerType`) that may hold an older one: storeReplacing for an owner that
  // may be a member.
  void storeIn(llvm::Value *owner, const Type *ownerType, llvm::Value *address, llvm::Value *value,
               const Type *type);
  // The value of the variable `var` at `address` (its slot or the field of
  // its box) becomes `value`, owned; `replacing` releases what it held.
  void storeVar(const VarSymbol *var, llvm::Value *address, llvm::Value *value, bool replacing);
  // The drop function of the objects of a cyclic type: what the fields hold
  // goes as from a member (releaseIn), and the memory unless it is one.
  void emitMemberDrop(const Type *type, CodeGen::ObjectLayout &layout);
  // At the start of the drop of an object that may have weak references.
  void forgetWeakRefs(llvm::Value *object);
  // releaseIn of a box or an environment that `owner` held.
  void releaseObjectIn(llvm::Value *owner, llvm::Value *object);

private:
  CodeGen &codegen_;
};

} // namespace tz
