#include "compiler/lower/library/stream/stream_lowerer/stream_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"

namespace tz {

bool StreamLowerer::isPlainObject(const Type *type) {
  return type->isObject() && !type->isClass() && !type->isAnyObject() && !type->isBytes() &&
         !type->isDate() && !type->isRegExp() && !type->isMatch() && !type->isIntl() &&
         !type->isCrypto() && !type->isRuntimeObject() && !type->isTimer() && type->bases().empty();
}

StreamLowerer::StreamLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *StreamLowerer::emitChunkAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  auto test = [&](llvm::Value *ok) {
    llvm::BasicBlock *next = this->lowering.functionFrame().newBlock("chunk.fits");
    this->lowering.builder.CreateCondBr(ok, next, fail);
    this->lowering.functionFrame().switchTo(next);
  };
  if (to->isBytes()) {
    llvm::Value *ok = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_lib_is_bytes", i32, {ptr, i32}),
        {u, this->lowering.builder.getInt32(uint32_t(to->bytes()))});
    test(this->lowering.builder.CreateICmpNE(ok, this->lowering.builder.getInt32(0)));
    this->lowering.refCounts().retainValue(u, to);
    return u;
  }
  if (StreamLowerer::isPlainObject(to)) {
    llvm::Value *layout = this->lowering.builder.CreateCall(
        this->lowering.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {u},
        "layout");
    test(this->lowering.builder.CreateIsNotNull(layout));
    llvm::Value *drop = this->lowering.builder.CreateLoad(ptr, layout, "drop");
    test(this->lowering.builder.CreateICmpEQ(drop,
                                             this->lowering.objectLayouts().objectLayout(to).drop));
    this->lowering.refCounts().retainValue(u, to);
    return u;
  }

  return this->lowering.unknownCastLowerer().emitUnknownAs(u, to, fail);
}

llvm::Value *StreamLowerer::emitStreamBuiltin(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  switch (call->builtin()) {
    case Builtin::LibChunk: {
      const Type *to = call->type;
      Lowering::Operand u = this->lowering.refCounts().emitOperand(call->args[0], true);
      if (to->isUnknown()) {
        this->lowering.refCounts().retainValue(u.value, to);
        llvm::Value *value = u.value;
        this->lowering.refCounts().dropOperand(u);
        return value;
      }
      llvm::BasicBlock *fail = this->lowering.functionFrame().newBlock("chunk.bad");
      llvm::Value *value = this->emitChunkAs(u.value, to, fail);
      llvm::BasicBlock *here = this->lowering.builder.GetInsertBlock();
      this->lowering.functionFrame().switchTo(fail);
      llvm::StringRef name = to->name();
      this->lowering.builder.CreateCall(
          this->lowering.unknownLowerer().unknownFunction(
              "tz_lib_bad_chunk", this->lowering.builder.getVoidTy(), {ptr, ptr, i32}),
          {u.value, this->lowering.stringLowerer().utf8Constant(name),
           this->lowering.builder.getInt32(uint32_t(name.size()))});
      this->lowering.builder.CreateUnreachable();
      this->lowering.builder.SetInsertPoint(here);
      this->lowering.refCounts().dropOperand(u);
      return value;
    }
    case Builtin::LibIsChunk: {
      const Type *wanted = call->args[1]->type;
      Lowering::Operand u = this->lowering.refCounts().emitOperand(call->args[0], true);
      if (wanted->isUnknown()) {
        this->lowering.refCounts().dropOperand(u);
        return this->lowering.builder.getTrue();
      }
      llvm::BasicBlock *fail = this->lowering.functionFrame().newBlock("chunk.not");
      llvm::BasicBlock *end = this->lowering.functionFrame().newBlock("chunk.end");
      llvm::Value *value = this->emitChunkAs(u.value, wanted, fail);
      this->lowering.refCounts().releaseValue(value, wanted);
      llvm::BasicBlock *fits = this->lowering.builder.GetInsertBlock();
      this->lowering.builder.CreateBr(end);
      this->lowering.functionFrame().switchTo(fail);
      this->lowering.builder.CreateBr(end);
      this->lowering.functionFrame().switchTo(end);
      llvm::PHINode *phi =
          this->lowering.builder.CreatePHI(this->lowering.builder.getInt1Ty(), 2, "is.chunk");
      phi->addIncoming(this->lowering.builder.getTrue(), fits);
      phi->addIncoming(this->lowering.builder.getFalse(), fail);
      this->lowering.refCounts().dropOperand(u);
      return phi;
    }
    case Builtin::LibOverrides: {
      const FunctionDecl *method = call->method() ? call->method()->decl : nullptr;
      Lowering::Operand object = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *result = this->lowering.builder.getFalse();
      if (method && method->slot() >= 0 && method->body) {
        result = this->lowering.builder.CreateICmpNE(
            this->lowering.inheritLowerer().vtableEntry(object.value, method->slot(), false),
            this->lowering.classDescriptors().methodCode(method), "overrides");
      }
      this->lowering.refCounts().dropOperand(object);
      return result;
    }
    case Builtin::LibFsOpen:
    case Builtin::LibFsRead:
    case Builtin::LibFsWrite:
    case Builtin::LibFsClose:
    case Builtin::LibStdinChunk: {
      llvm::Type *voidTy = this->lowering.builder.getVoidTy();
      llvm::Type *i64 = this->lowering.builder.getInt64Ty();
      llvm::Type *f64 = this->lowering.builder.getDoubleTy();
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      llvm::Value *promise = this->lowering.promiseLowerer().newPromise(call->type->promised());
      auto run = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                     std::vector<llvm::Value *> args, bool maker) {
        params.insert(params.begin(), ptr);
        args.insert(args.begin(), promise);
        if (maker) {
          params.push_back(ptr);
          args.push_back(this->lowering.fsLowerer().errorMaker(call->type->promised()->errType()));
        }
        this->lowering.builder.CreateCall(
            this->lowering.processLowerer().processFunction(name, voidTy, params), args);
      };
      switch (call->builtin()) {
        case Builtin::LibFsOpen:
          run("tz_fss_open", {ptr, i64, ptr, i64, i64},
              {this->lowering.stringLowerer().stringObj(values[0]),
               this->lowering.stringLowerer().stringMeta(values[0]),
               this->lowering.stringLowerer().stringObj(values[1]),
               this->lowering.stringLowerer().stringMeta(values[1]), values[2]},
              true);
          break;
        case Builtin::LibFsRead:
        case Builtin::LibFsWrite:
          run(call->builtin() == Builtin::LibFsRead ? "tz_fss_read" : "tz_fss_write",
              {i64, ptr, i64, i64, f64}, {values[0], values[1], values[2], values[3], values[4]},
              true);
          break;
        case Builtin::LibFsClose:
          run("tz_fss_close", {i64, i32},
              {values[0], this->lowering.builder.CreateZExt(values[1], i32)}, true);
          break;
        default:
          run("tz_stdin_chunk", {i64}, {values[0]}, false);
          break;
      }
      for (size_t i = 0; i < values.size(); ++i) {
        this->lowering.refCounts().releaseValue(values[i], call->args[i]->type);
      }
      return promise;
    }
    case Builtin::LibStdWrite: {
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      llvm::Value *gone = this->lowering.builder.CreateCall(
          this->lowering.processLowerer().processFunction(
              "tz_lib_std_write_checked", this->lowering.builder.getInt64Ty(),
              {this->lowering.builder.getInt64Ty(), ptr}),
          {values[0], values[1]});
      this->lowering.refCounts().releaseValue(values[1], call->args[1]->type);
      return gone;
    }
    case Builtin::LibStdinFailed: {
      llvm::Value *failed =
          this->lowering.builder.CreateCall(this->lowering.processLowerer().processFunction(
              "tz_stdin_failed", this->lowering.builder.getInt32Ty(), {}));
      return this->lowering.builder.CreateICmpNE(failed, this->lowering.builder.getInt32(0));
    }
    default:
      return nullptr;
  }
}

}
