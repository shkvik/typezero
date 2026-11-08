#include "compiler/lower/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/stream/stream_lowerer/stream_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"

using llvm::cast;

namespace tz {

LibraryCallLowerer::LibraryCallLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *LibraryCallLowerer::emitLibBuiltin(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  switch (call->builtin()) {
    case Builtin::LibListener: {
      Lowering::Operand box = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *value =
          this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(call->type),
                                            this->lowering.builder.CreateConstInBoundsGEP1_64(
                                                this->lowering.builder.getInt8Ty(), box.value, 24),
                                            "listener");
      this->lowering.refCounts().retainValue(value, call->type);
      this->lowering.refCounts().dropOperand(box);
      return value;
    }
    case Builtin::LibBoundListener: {
      Lowering::Operand box = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *value =
          this->lowering.builder.CreateLoad(this->lowering.moduleBuilder().lower(call->type),
                                            this->lowering.builder.CreateConstInBoundsGEP1_64(
                                                this->lowering.builder.getInt8Ty(), box.value, 24),
                                            "listener");
      this->lowering.refCounts().dropOperand(box);
      llvm::Value *receiver = this->lowering.exprLowerer().emitExpr(call->args[1]);
      return this->lowering.builder.CreateInsertValue(value, receiver, 1);
    }
    case Builtin::LibAs: {
      const Type *from = call->args[0]->type;
      const Type *wanted = call->type->nonOptional();
      llvm::Value *object = this->lowering.exprLowerer().emitExpr(call->args[0]);
      if (from->isSubtypeOf(wanted)) {
        return object;
      }
      const ClassDecl *cls = wanted->classDecl();
      llvm::Value *descriptor =
          this->lowering.builder.CreateLoad(ptr,
                                            this->lowering.builder.CreateConstInBoundsGEP1_64(
                                                this->lowering.builder.getInt8Ty(), object, 8),
                                            "descriptor");
      llvm::Value *id = this->lowering.builder.CreateLoad(
          this->lowering.builder.getInt32Ty(),
          this->lowering.builder.CreateStructGEP(this->lowering.layoutDescTy, descriptor, 6),
          "class");
      llvm::Value *matches =
          cls->lastId <= cls->id
              ? this->lowering.builder.CreateICmpEQ(id, this->lowering.builder.getInt32(cls->id))
              : this->lowering.builder.CreateICmpULE(
                    this->lowering.builder.CreateSub(id, this->lowering.builder.getInt32(cls->id)),
                    this->lowering.builder.getInt32(cls->lastId - cls->id));
      llvm::BasicBlock *dropBlock = this->lowering.functionFrame().newBlock("as.drop");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("as.end");
      this->lowering.builder.CreateCondBr(matches, endBlock, dropBlock);
      llvm::BasicBlock *entry = this->lowering.builder.GetInsertBlock();
      this->lowering.functionFrame().switchTo(dropBlock);
      this->lowering.refCounts().releaseValue(object, from);
      llvm::BasicBlock *dropEnd = this->lowering.builder.GetInsertBlock();
      this->lowering.builder.CreateBr(endBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      llvm::PHINode *phi = this->lowering.builder.CreatePHI(ptr, 2, "as");
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
    case Builtin::LibStdinFailed:
    case Builtin::LibStdWrite:
      return this->lowering.streamLowerer().emitStreamBuiltin(call);
    case Builtin::LibAhead:
    case Builtin::LibAheadMovable:
      return this->lowering.exprLowerer().emitExpr(call->args[0]);
    case Builtin::LibPanic: {
      llvm::Value *text = this->lowering.exprLowerer().emitExpr(call->args[0]);
      llvm::FunctionCallee panic = this->lowering.processLowerer().processFunction(
          "tz_panic_str", this->lowering.builder.getVoidTy(),
          {ptr, this->lowering.builder.getInt64Ty()});
      cast<llvm::Function>(panic.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
      this->lowering.builder
          .CreateCall(panic, {this->lowering.stringLowerer().stringObj(text),
                              this->lowering.stringLowerer().stringMeta(text)})
          ->setDoesNotReturn();
      this->lowering.builder.CreateUnreachable();
      this->lowering.functionFrame().startDeadBlock();
      return nullptr;
    }
    case Builtin::LibDefer: {
      llvm::Value *run = this->lowering.exprLowerer().emitExpr(call->args[0]);
      return this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_defer_new", ptr, {ptr, ptr}),
          {this->lowering.builder.CreateExtractValue(run, 0),
           this->lowering.builder.CreateExtractValue(run, 1)},
          "defer");
    }
    case Builtin::LibWithContext: {
      Lowering::Operand frame = this->lowering.refCounts().emitOperand(call->args[0], true);
      Lowering::Operand callback = this->lowering.refCounts().emitOperand(call->args[1], true);
      llvm::Value *outer = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction("tz_context_enter", ptr, {ptr}),
          {frame.value}, "outer");
      llvm::Value *result = this->lowering.closureLowerer().callFunctionValue(
          callback.value, call->args[1]->type, {});
      this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction(
              "tz_context_leave", this->lowering.builder.getVoidTy(), {ptr}),
          {outer});
      this->lowering.refCounts().dropOperand(callback);
      this->lowering.refCounts().dropOperand(frame);
      return result;
    }
    case Builtin::LibOrder:
      if (this->lowering.parallel_) {
        this->lowering.builder.CreateCall(this->lowering.processLowerer().processFunction(
            "tz_order_enter", this->lowering.builder.getVoidTy(), {}));
      }
      return nullptr;
    default:
      return nullptr;
  }
}

}
