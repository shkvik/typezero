#include "compiler/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;

namespace tz {

FsPromisesLowerer::FsPromisesLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *FsPromisesLowerer::emitFsPromise(CallExpr *call) {
  llvm::Type *voidTy = this->lowering.builder_.getVoidTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  const Type *settled = call->type->promised();
  const Type *value = settled->isResult() ? settled->okType() : settled;
  llvm::Value *promise = this->lowering.promiseLowerer().newPromise(settled);
  auto obj = [&](size_t i) { return this->lowering.stringLowerer().stringObj(values[i]); };
  auto meta = [&](size_t i) { return this->lowering.stringLowerer().stringMeta(values[i]); };
  auto flag = [&](size_t i) { return this->lowering.builder_.CreateZExt(values[i], i32); };
  auto run = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                 std::vector<llvm::Value *> args) {
    params.insert(params.begin(), ptr);
    args.insert(args.begin(), promise);
    params.push_back(ptr);
    args.push_back(this->lowering.fsLowerer().errorMaker(settled->errType()));
    this->lowering.builder_.CreateCall(
        this->lowering.processLowerer().processFunction(name, voidTy, params), args);
  };
  switch (call->builtin()) {
    case Builtin::FspReadFile:
      run("tz_fsp_read_file", {ptr, i64, ptr, i64, i64},
          {obj(0), meta(0), obj(1), meta(1),
           value->isBytes() ? llvm::ConstantInt::getSigned(i64, -1) : values[2]});
      break;
    case Builtin::FspWriteFile:
      if (!call->args[1]->type->isString()) {
        run("tz_fsp_write_file_bytes", {ptr, i64, ptr, ptr, i64, i64},
            {obj(0), meta(0), values[1], obj(2), meta(2), values[3]});
      } else {
        run("tz_fsp_write_file", {ptr, i64, ptr, i64, ptr, i64, i64, i64},
            {obj(0), meta(0), obj(1), meta(1), obj(2), meta(2), values[3], values[4]});
      }
      break;
    case Builtin::FspAccess:
    case Builtin::FspChmod:
      run(call->builtin() == Builtin::FspAccess ? "tz_fsp_access" : "tz_fsp_chmod", {ptr, i64, i64},
          {obj(0), meta(0), values[1]});
      break;
    case Builtin::FspTruncate:
      run("tz_fsp_truncate", {ptr, i64, i64}, {obj(0), meta(0), values[1]});
      break;
    case Builtin::FspStat:
      run("tz_fsp_stat", {ptr, i64, i32, ptr},
          {obj(0), meta(0), flag(1), this->lowering.fsLowerer().objectMaker(value)});
      break;
    case Builtin::FspReaddir: {
      const Type *element = value->inner();
      llvm::Value *dirent =
          element->isClass()
              ? static_cast<llvm::Value *>(this->lowering.fsLowerer().objectMaker(element))
              : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
      run("tz_fsp_readdir", {ptr, i64, i32, ptr, ptr},
          {obj(0), meta(0), flag(1), this->lowering.typeDescriptors().typeDescriptor(element),
           dirent});
      break;
    }
    case Builtin::FspMkdir:
      run("tz_fsp_mkdir", {ptr, i64, i32, i64}, {obj(0), meta(0), flag(1), values[2]});
      break;
    case Builtin::FspMkdtemp:
      run("tz_fsp_mkdtemp", {ptr, i64}, {obj(0), meta(0)});
      break;
    case Builtin::FspRmdir:
    case Builtin::FspUnlink:
      run("tz_fsp_remove", {ptr, i64, i32},
          {obj(0), meta(0),
           this->lowering.builder_.getInt32(call->builtin() == Builtin::FspUnlink)});
      break;
    case Builtin::FspRm:
      run("tz_fsp_rm", {ptr, i64, i32, i32}, {obj(0), meta(0), flag(1), flag(2)});
      break;
    case Builtin::FspRename:
    case Builtin::FspReplace:
    case Builtin::FspCopyFile:
      run(call->builtin() == Builtin::FspRename    ? "tz_fsp_rename"
          : call->builtin() == Builtin::FspReplace ? "tz_fsp_replace"
                                                   : "tz_fsp_copy_file",
          {ptr, i64, ptr, i64}, {obj(0), meta(0), obj(1), meta(1)});
      break;
    case Builtin::FspRealpath:
      run("tz_fsp_realpath", {ptr, i64}, {obj(0), meta(0)});
      break;
    default:
      llvm_unreachable("not a function of node:fs/promises");
  }
  for (size_t i = 0; i < values.size(); ++i) {
    this->lowering.refCounts().releaseValue(values[i], call->args[i]->type);
  }

  return promise;
}

llvm::Value *FsPromisesLowerer::emitReadlineBuiltin(CallExpr *call) {
  llvm::Type *voidTy = this->lowering.builder_.getVoidTy();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  if (call->builtin() == Builtin::RlCreate) {
    std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
    return this->lowering.builder_.CreateCall(
        this->lowering.processLowerer().processFunction("tz_readline_new", ptr, {i64}), {values[0]},
        "readline");
  }
  Lowering::Operand rl =
      this->lowering.refCounts().emitOperand(cast<MemberExpr>(call->callee)->object, true);
  llvm::Value *result = nullptr;
  switch (call->builtin()) {
    case Builtin::RlQuestion: {
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      result = this->lowering.promiseLowerer().newPromise(call->type->promised());
      this->lowering.builder_.CreateCall(
          this->lowering.processLowerer().processFunction("tz_readline_question", voidTy,
                                                          {ptr, ptr, ptr, i64, ptr}),
          {rl.value, result, this->lowering.stringLowerer().stringObj(values[0]),
           this->lowering.stringLowerer().stringMeta(values[0]),
           this->lowering.fsLowerer().objectMaker(call->type->promised()->errType())});
      for (size_t i = 0; i < values.size(); ++i) {
        this->lowering.refCounts().releaseValue(values[i], call->args[i]->type);
      }
      break;
    }
    case Builtin::RlQuestionCallback: {
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      llvm::Value *answer = this->lowering.promiseLowerer().newPromise(Type::string());
      this->lowering.builder_.CreateCall(
          this->lowering.processLowerer().processFunction("tz_readline_question", voidTy,
                                                          {ptr, ptr, ptr, i64, ptr}),
          {rl.value, answer, this->lowering.stringLowerer().stringObj(values[0]),
           this->lowering.stringLowerer().stringMeta(values[0]),
           llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy())});
      llvm::Value *done = this->lowering.promiseLowerer().newPromise(Type::voidTy());
      const Type *callback = call->args[1]->type;
      this->lowering.reactionLowerer().attachReaction(
          answer, done,
          this->lowering.reactionLowerer().reactionHandler(Type::string(), Type::voidTy(),
                                                           callback),
          values[1]);
      this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {done});
      this->lowering.builder_.CreateCall(this->lowering.instanceReleaseFn_, {answer});
      this->lowering.refCounts().releaseValue(values[0], call->args[0]->type);
      break;
    }
    case Builtin::RlClose:
      this->lowering.builder_.CreateCall(
          this->lowering.processLowerer().processFunction("tz_readline_close", voidTy, {ptr}),
          {rl.value});
      break;
    default:
      llvm_unreachable("not a built-in of node:readline");
  }
  this->lowering.refCounts().dropOperand(rl);

  return result;
}

llvm::Value *FsPromisesLowerer::emitAsyncIoBuiltin(CallExpr *call) {
  llvm::Type *voidTy = this->lowering.builder_.getVoidTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Type *f64 = this->lowering.builder_.getDoubleTy();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  switch (call->builtin()) {
    case Builtin::TimersSetTimeout:
    case Builtin::TimersSetImmediate:
    case Builtin::TimersSetInterval: {
      std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
      bool immediate = call->builtin() == Builtin::TimersSetImmediate;
      llvm::Value *delay = immediate ? llvm::ConstantFP::get(f64, 0.0) : values[0];
      size_t at = immediate ? 0 : 1;
      const Type *value = call->builtin() == Builtin::TimersSetInterval ? call->type->yielded()
                                                                        : call->type->promised();
      bool holds = call->args.size() - at == 2;
      llvm::Value *given = holds ? values[at] : nullptr;
      llvm::Value *ref = this->lowering.builder_.CreateZExt(values.back(), i32);
      llvm::Value *result;
      if (call->builtin() == Builtin::TimersSetInterval) {
        llvm::Value *slot =
            given ? this->lowering.functionFrame().spill(given, "interval.value")
                  : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
        llvm::Value *type =
            this->lowering.promiseLowerer().promiseValueSize(value)
                ? static_cast<llvm::Value *>(this->lowering.typeDescriptors().typeDescriptor(value))
                : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
        result =
            this->lowering.builder_.CreateCall(this->lowering.processLowerer().processFunction(
                                                   "tz_timers_interval", ptr, {f64, ptr, ptr, i32}),
                                               {delay, slot, type, ref}, "interval");
      } else {
        result = this->lowering.promiseLowerer().newPromise(value);
        if (given && this->lowering.promiseLowerer().promiseValueSize(value)) {
          this->lowering.builder_.CreateStore(
              given, this->lowering.promiseLowerer().promiseValueAddress(result));
        }
        this->lowering.builder_.CreateCall(
            this->lowering.processLowerer().processFunction("tz_timers_wait", voidTy,
                                                            {ptr, f64, i32, i32}),
            {result, delay, this->lowering.builder_.getInt32(immediate), ref});
      }
      return result;
    }
    default:
      llvm_unreachable("not a built-in of waiting for I/O");
  }
}

}
