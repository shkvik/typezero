#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/stream/stream_lowerer/stream_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

LibraryCallLowerer::LibraryCallLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *LibraryCallLowerer::emitLibBuiltin(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  switch (call->builtin) {
  case Builtin::LibListener: {
    // The function lies in its box after the header (TZ_BOX_VALUE in
    // runtime/core/unknown/unknown.h), the box of an unknown that holds it; retained,
    // as the listener may remove itself while it runs.
    Lowering::Operand box = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *value =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(call->type),
                                      lowering_.builder_.CreateConstInBoundsGEP1_64(
                                          lowering_.builder_.getInt8Ty(), box.value, 24),
                                      "listener");
    lowering_.refCounts().retainValue(value, call->type);
    lowering_.refCounts().dropOperand(box);
    return value;
  }
  case Builtin::LibAs: {
    // The object, if its class is C or one down its chain, as `instanceof`
    // compares; otherwise undefined, and the reference goes.
    const Type *from = call->args[0]->type;
    const Type *wanted = call->type->nonOptional();
    llvm::Value *object = lowering_.exprLowerer().emitExpr(call->args[0]);
    if (from->isSubtypeOf(wanted)) {
      return object;
    }
    const ClassDecl *cls = wanted->classDecl();
    llvm::Value *descriptor = lowering_.builder_.CreateLoad(
        ptr,
        lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), object, 8),
        "descriptor");
    llvm::Value *id = lowering_.builder_.CreateLoad(
        lowering_.builder_.getInt32Ty(),
        lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, descriptor, 6), "class");
    llvm::Value *matches =
        cls->lastId <= cls->id
            ? lowering_.builder_.CreateICmpEQ(id, lowering_.builder_.getInt32(cls->id))
            : lowering_.builder_.CreateICmpULE(
                  lowering_.builder_.CreateSub(id, lowering_.builder_.getInt32(cls->id)),
                  lowering_.builder_.getInt32(cls->lastId - cls->id));
    llvm::BasicBlock *dropBlock = lowering_.functionFrame().newBlock("as.drop");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("as.end");
    lowering_.builder_.CreateCondBr(matches, endBlock, dropBlock);
    llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
    lowering_.functionFrame().switchTo(dropBlock);
    lowering_.refCounts().releaseValue(object, from);
    llvm::BasicBlock *dropEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = lowering_.builder_.CreatePHI(ptr, 2, "as");
    phi->addIncoming(object, entry);
    phi->addIncoming(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), dropEnd);
    return phi;
  }
  case Builtin::LibChunk:
  case Builtin::LibIsChunk:
  case Builtin::LibOverrides:
  case Builtin::LibFsOpen:
  case Builtin::LibFsRead:
  case Builtin::LibFsWrite:
  case Builtin::LibFsClose:
  case Builtin::LibStdinChunk:
  case Builtin::LibStdWrite:
    return lowering_.streamLowerer().emitStreamBuiltin(call);
  case Builtin::LibAhead: // the promise itself; emitAwait makes the site
    return lowering_.exprLowerer().emitExpr(call->args[0]);
  case Builtin::LibOrder:
    if (lowering_.parallel_) {
      lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
          "tz_order_enter", lowering_.builder_.getVoidTy(), {}));
    }
    return nullptr;
  case Builtin::LibNativeSignal: {
    // The runtime's side of the signal, retained, or null for undefined.
    const Type *type = call->args[0]->type;
    Lowering::Operand signal = lowering_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *native = nativeSignal(signal.value, type, /*retain=*/true);
    lowering_.refCounts().dropOperand(signal);
    return native;
  }
  default:
    return nullptr;
  }
}

bool LibraryCallLowerer::isAbortSignal(const Type *type) {
  const ClassDecl *cls = type->nonOptional()->classDecl();

  return cls && cls->library && cls->name == "AbortSignal";
}

llvm::Value *LibraryCallLowerer::nativeSignal(llvm::Value *signal, const Type *type, bool retain) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Constant *null = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr));
  if (!isAbortSignal(type)) {
    return null;
  }
  llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *loadBlock = lowering_.functionFrame().newBlock("signal.native");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("signal.end");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(signal), loadBlock, endBlock);
  lowering_.functionFrame().switchTo(loadBlock);
  llvm::Value *native = lowering_.builder_.CreateLoad(
      ptr, lowering_.objectLayouts().fieldAddress(signal, type->nonOptional(), "#native"),
      "native");
  if (retain) {
    lowering_.refCounts().retainValue(native, Type::runtimeObject(Type::Runtime::AbortSignal));
  }
  llvm::BasicBlock *loaded = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(ptr, 2, "native");
  phi->addIncoming(null, entry);
  phi->addIncoming(native, loaded);

  return phi;
}

} // namespace tz
