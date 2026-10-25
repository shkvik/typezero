#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/stream/stream_lowerer/stream_lowerer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

LibraryCallLowerer::LibraryCallLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *LibraryCallLowerer::emitLibBuiltin(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  switch (call->builtin) {
  case Builtin::LibListener: {
    // The function lies in its box after the header (TZ_BOX_VALUE in
    // runtime/core/tzrt/tzrt.h), the box of an unknown that holds it; retained,
    // as the listener may remove itself while it runs.
    CodeGen::Operand box = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *value = codegen_.builder_.CreateLoad(
        codegen_.moduleBuilder().lower(call->type), codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), box.value, 24),
        "listener");
    codegen_.refCounts().retainValue(value, call->type);
    codegen_.refCounts().dropOperand(box);
    return value;
  }
  case Builtin::LibAs: {
    // The object, if its class is C or one down its chain, as `instanceof`
    // compares; otherwise undefined, and the reference goes.
    const Type *from = call->args[0]->type;
    const Type *wanted = call->type->nonOptional();
    llvm::Value *object = codegen_.exprLowerer().emitExpr(call->args[0]);
    if (from->isSubtypeOf(wanted)) {
      return object;
    }
    const ClassDecl *cls = wanted->classDecl();
    llvm::Value *descriptor = codegen_.builder_.CreateLoad(
        ptr, codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 8), "descriptor");
    llvm::Value *id = codegen_.builder_.CreateLoad(
        codegen_.builder_.getInt32Ty(), codegen_.builder_.CreateStructGEP(codegen_.layoutDescTy_, descriptor, 6), "class");
    llvm::Value *matches =
        cls->lastId <= cls->id
            ? codegen_.builder_.CreateICmpEQ(id, codegen_.builder_.getInt32(cls->id))
            : codegen_.builder_.CreateICmpULE(codegen_.builder_.CreateSub(id, codegen_.builder_.getInt32(cls->id)),
                                     codegen_.builder_.getInt32(cls->lastId - cls->id));
    llvm::BasicBlock *dropBlock = codegen_.functionFrame().newBlock("as.drop");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("as.end");
    codegen_.builder_.CreateCondBr(matches, endBlock, dropBlock);
    llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
    codegen_.functionFrame().switchTo(dropBlock);
    codegen_.refCounts().releaseValue(object, from);
    llvm::BasicBlock *dropEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(ptr, 2, "as");
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
    return codegen_.streamLowerer().emitStreamBuiltin(call);
  case Builtin::LibAhead: // the promise itself; emitAwait makes the site
    return codegen_.exprLowerer().emitExpr(call->args[0]);
  case Builtin::LibOrder:
    if (codegen_.parallel_) {
      codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_order_enter", codegen_.builder_.getVoidTy(), {}));
    }
    return nullptr;
  case Builtin::LibNativeSignal: {
    // The runtime's side of the signal, retained, or null for undefined.
    const Type *type = call->args[0]->type;
    CodeGen::Operand signal = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *native = nativeSignal(signal.value, type, /*retain=*/true);
    codegen_.refCounts().dropOperand(signal);
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
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Constant *null = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr));
  if (!isAbortSignal(type)) {
    return null;
  }
  llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
  llvm::BasicBlock *loadBlock = codegen_.functionFrame().newBlock("signal.native");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("signal.end");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(signal), loadBlock, endBlock);
  codegen_.functionFrame().switchTo(loadBlock);
  llvm::Value *native =
      codegen_.builder_.CreateLoad(ptr, codegen_.objectLayouts().fieldAddress(signal, type->nonOptional(), "#native"), "native");
  if (retain) {
    codegen_.refCounts().retainValue(native, Type::runtimeObject(Type::Runtime::AbortSignal));
  }
  llvm::BasicBlock *loaded = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);
  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(ptr, 2, "native");
  phi->addIncoming(null, entry);
  phi->addIncoming(native, loaded);

  return phi;
}

} // namespace tz
