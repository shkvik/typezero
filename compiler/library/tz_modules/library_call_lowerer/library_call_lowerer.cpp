#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/stream/stream_lowerer/stream_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"

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
    Lowering::Operand box = lowering_.refCounts().emitOperand(call->args[0], true);
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
  case Builtin::LibAhead:
    return lowering_.exprLowerer().emitExpr(call->args[0]);
  case Builtin::LibPanic: {
    llvm::Value *text = lowering_.exprLowerer().emitExpr(call->args[0]);
    llvm::FunctionCallee panic = lowering_.processLowerer().processFunction(
        "tz_panic_str", lowering_.builder_.getVoidTy(), {ptr, lowering_.builder_.getInt64Ty()});
    cast<llvm::Function>(panic.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
    lowering_.builder_
        .CreateCall(panic, {lowering_.stringLowerer().stringObj(text),
                            lowering_.stringLowerer().stringMeta(text)})
        ->setDoesNotReturn();
    lowering_.builder_.CreateUnreachable();
    lowering_.functionFrame().startDeadBlock();
    return nullptr;
  }
  case Builtin::LibDefer: {
    llvm::Value *run = lowering_.exprLowerer().emitExpr(call->args[0]);
    return lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_defer_new", ptr, {ptr, ptr}),
        {lowering_.builder_.CreateExtractValue(run, 0),
         lowering_.builder_.CreateExtractValue(run, 1)},
        "defer");
  }
  case Builtin::LibOrder:
    if (lowering_.parallel_) {
      lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
          "tz_order_enter", lowering_.builder_.getVoidTy(), {}));
    }
    return nullptr;
  default:
    return nullptr;
  }
}

}
