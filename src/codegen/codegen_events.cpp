// The intrinsics of the built-in modules tz has written in tz (src/lib)
// that take a type, which Sema gives as Builtin::LibListener and LibAs (see
// src/sema/sema_events.cpp): `__tz_listener<F>(u)`, which takes the function
// out of the box of an unknown, and `__tz_as<C>(o)`, a comparison of class
// numbers; and the runtime's side of an AbortSignal given as a `signal`, a
// field. The intrinsics the code declares are thunks (codegen_library.cpp).

#include "codegen/codegen.h"

namespace tz {

llvm::Value *CodeGen::emitLibBuiltin(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy();
  switch (call->builtin) {
  case Builtin::LibListener: {
    // The function lies in its box after the header (TZ_BOX_VALUE in
    // runtime/tzrt.h), the box of an unknown that holds it; retained, as
    // the listener may remove itself while it runs.
    Operand box = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *value = builder_.CreateLoad(
        lower(call->type), builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), box.value, 24),
        "listener");
    retainValue(value, call->type);
    dropOperand(box);
    return value;
  }
  case Builtin::LibAs: {
    // The object, if its class is C or one down its chain, as `instanceof`
    // compares; otherwise undefined, and the reference goes.
    const Type *from = call->args[0]->type;
    const Type *wanted = call->type->nonOptional();
    llvm::Value *object = emitExpr(call->args[0]);
    if (from->isSubtypeOf(wanted))
      return object;
    const ClassDecl *cls = wanted->classDecl();
    llvm::Value *descriptor = builder_.CreateLoad(
        ptr, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 8), "descriptor");
    llvm::Value *id = builder_.CreateLoad(
        builder_.getInt32Ty(), builder_.CreateStructGEP(layoutDescTy_, descriptor, 6), "class");
    llvm::Value *matches =
        cls->lastId <= cls->id
            ? builder_.CreateICmpEQ(id, builder_.getInt32(cls->id))
            : builder_.CreateICmpULE(builder_.CreateSub(id, builder_.getInt32(cls->id)),
                                     builder_.getInt32(cls->lastId - cls->id));
    llvm::BasicBlock *dropBlock = newBlock("as.drop");
    llvm::BasicBlock *endBlock = newBlock("as.end");
    builder_.CreateCondBr(matches, endBlock, dropBlock);
    llvm::BasicBlock *entry = builder_.GetInsertBlock();
    switchTo(dropBlock);
    releaseValue(object, from);
    llvm::BasicBlock *dropEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    llvm::PHINode *phi = builder_.CreatePHI(ptr, 2, "as");
    phi->addIncoming(object, entry);
    phi->addIncoming(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), dropEnd);
    return phi;
  }
  case Builtin::LibNativeSignal: {
    // The runtime's side of the signal, retained, or null for undefined.
    const Type *type = call->args[0]->type;
    Operand signal = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *native = nativeSignal(signal.value, type, /*retain=*/true);
    dropOperand(signal);
    return native;
  }
  default:
    return nullptr;
  }
}

bool CodeGen::isAbortSignal(const Type *type) {
  const ClassDecl *cls = type->nonOptional()->classDecl();
  return cls && cls->library && cls->name == "AbortSignal";
}

llvm::Value *CodeGen::nativeSignal(llvm::Value *signal, const Type *type, bool retain) {
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Constant *null = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr));
  if (!isAbortSignal(type))
    return null;
  llvm::BasicBlock *entry = builder_.GetInsertBlock();
  llvm::BasicBlock *loadBlock = newBlock("signal.native");
  llvm::BasicBlock *endBlock = newBlock("signal.end");
  builder_.CreateCondBr(builder_.CreateIsNotNull(signal), loadBlock, endBlock);
  switchTo(loadBlock);
  llvm::Value *native =
      builder_.CreateLoad(ptr, fieldAddress(signal, type->nonOptional(), "#native"), "native");
  if (retain)
    retainValue(native, Type::runtimeObject(Type::Runtime::AbortSignal));
  llvm::BasicBlock *loaded = builder_.GetInsertBlock();
  builder_.CreateBr(endBlock);
  switchTo(endBlock);
  llvm::PHINode *phi = builder_.CreatePHI(ptr, 2, "native");
  phi->addIncoming(null, entry);
  phi->addIncoming(native, loaded);
  return phi;
}

} // namespace tz
