#include "compiler/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/builtin_modules/builtin_module_lowerer/builtin_module_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

FsPromisesLowerer::FsPromisesLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *FsPromisesLowerer::emitFsPromise(CallExpr *call) {
  llvm::Type *voidTy = codegen_.builder_.getVoidTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Type *i64 = codegen_.builder_.getInt64Ty(), *ptr = codegen_.builder_.getPtrTy();
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
  const Type *value = call->type->promised();
  llvm::Value *promise = codegen_.promiseLowerer().newPromise(value);
  auto obj = [&](size_t i) { return codegen_.stringLowerer().stringObj(values[i]); };
  auto meta = [&](size_t i) { return codegen_.stringLowerer().stringMeta(values[i]); };
  auto flag = [&](size_t i) { return codegen_.builder_.CreateZExt(values[i], i32); };
  // The runtime function, with the promise first and the maker of
  // SystemError last.
  auto run = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                 std::vector<llvm::Value *> args) {
    params.insert(params.begin(), ptr);
    args.insert(args.begin(), promise);
    params.push_back(ptr);
    args.push_back(codegen_.builtinModuleLowerer().systemErrorMaker());
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction(name, voidTy, params), args);
  };
  switch (call->builtin) {
  case Builtin::FspReadFile:
    // The path, the flag, the signal, then the encoding of text; without
    // one, a Buffer (-1).
    run("tz_fsp_read_file", {ptr, i64, ptr, i64, ptr, i32},
        {obj(0), meta(0), obj(1), meta(1), values[2],
         value->isBytes() ? llvm::ConstantInt::getSigned(i32, -1) : values[3]});
    break;
  case Builtin::FspWriteFile:
    // The path, the data, the flag, the mode, the encoding of text, the
    // signal; bytes have no encoding.
    if (!call->args[1]->type->isString()) {
      run("tz_fsp_write_file_bytes", {ptr, i64, ptr, ptr, i64, i32, ptr},
          {obj(0), meta(0), values[1], obj(2), meta(2), values[3], values[4]});
    } else {
      run("tz_fsp_write_file", {ptr, i64, ptr, i64, ptr, i64, i32, ptr, i32},
          {obj(0), meta(0), obj(1), meta(1), obj(2), meta(2), values[3], values[5], values[4]});
    }
    break;
  case Builtin::FspAccess:
  case Builtin::FspChmod:
    run(call->builtin == Builtin::FspAccess ? "tz_fsp_access" : "tz_fsp_chmod", {ptr, i64, i32},
        {obj(0), meta(0), values[1]});
    break;
  case Builtin::FspTruncate:
    run("tz_fsp_truncate", {ptr, i64, i64}, {obj(0), meta(0), values[1]});
    break;
  case Builtin::FspStat:
    run("tz_fsp_stat", {ptr, i64, i32, ptr}, {obj(0), meta(0), flag(1), codegen_.builtinModuleLowerer().objectMaker(value)});
    break;
  case Builtin::FspReaddir: {
    const Type *element = value->inner();
    llvm::Value *dirent = element->isClass() ? static_cast<llvm::Value *>(codegen_.builtinModuleLowerer().objectMaker(element))
                                             : llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
    run("tz_fsp_readdir", {ptr, i64, i32, ptr, ptr},
        {obj(0), meta(0), flag(1), codegen_.typeDescriptors().typeDescriptor(element), dirent});
    break;
  }
  case Builtin::FspMkdir:
    run("tz_fsp_mkdir", {ptr, i64, i32, i32}, {obj(0), meta(0), flag(1), values[2]});
    break;
  case Builtin::FspMkdtemp:
    run("tz_fsp_mkdtemp", {ptr, i64}, {obj(0), meta(0)});
    break;
  case Builtin::FspRmdir:
  case Builtin::FspUnlink:
    run("tz_fsp_remove", {ptr, i64, i32},
        {obj(0), meta(0), codegen_.builder_.getInt32(call->builtin == Builtin::FspUnlink)});
    break;
  case Builtin::FspRm:
    run("tz_fsp_rm", {ptr, i64, i32, i32}, {obj(0), meta(0), flag(1), flag(2)});
    break;
  case Builtin::FspRename:
    run("tz_fsp_rename", {ptr, i64, ptr, i64}, {obj(0), meta(0), obj(1), meta(1)});
    break;
  case Builtin::FspCopyFile:
    run("tz_fsp_copy_file", {ptr, i64, ptr, i64, i32},
        {obj(0), meta(0), obj(1), meta(1), values[2]});
    break;
  case Builtin::FspRealpath:
    run("tz_fsp_realpath", {ptr, i64}, {obj(0), meta(0)});
    break;
  default:
    llvm_unreachable("not a function of node:fs/promises");
  }
  for (size_t i = 0; i < values.size(); ++i) {
    codegen_.refCounts().releaseValue(values[i], call->args[i]->type);
  }

  return promise;
}

llvm::Value *FsPromisesLowerer::emitReadlineBuiltin(CallExpr *call) {
  llvm::Type *voidTy = codegen_.builder_.getVoidTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Type *i64 = codegen_.builder_.getInt64Ty(), *ptr = codegen_.builder_.getPtrTy();
  if (call->builtin == Builtin::RlCreate) {
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    llvm::Value *rl = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_readline_new", ptr, {i32, ptr}),
                                          {values[0], values[1]}, "readline");
    codegen_.refCounts().releaseValue(values[1], call->args[1]->type);
    return rl;
  }
  CodeGen::Operand rl = codegen_.refCounts().emitOperand(cast<MemberExpr>(call->callee)->object, /*canBorrow=*/true);
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::RlQuestion: {
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    result = codegen_.promiseLowerer().newPromise(Type::string());
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_readline_question", voidTy, {ptr, ptr, ptr, i64, ptr}),
                        {rl.value, result, codegen_.stringLowerer().stringObj(values[0]), codegen_.stringLowerer().stringMeta(values[0]), values[1]});
    for (size_t i = 0; i < values.size(); ++i) {
      codegen_.refCounts().releaseValue(values[i], call->args[i]->type);
    }
    break;
  }
  case Builtin::RlQuestionCallback: {
    // The question of node:readline/promises, whose answer goes to the
    // callback as then() would give it; an error in it, or a question of an
    // interface closed already, ends the program.
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    llvm::Value *answer = codegen_.promiseLowerer().newPromise(Type::string());
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_readline_question", voidTy, {ptr, ptr, ptr, i64, ptr}),
                        {rl.value, answer, codegen_.stringLowerer().stringObj(values[0]), codegen_.stringLowerer().stringMeta(values[0]),
                         llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy())});
    llvm::Value *done = codegen_.promiseLowerer().newPromise(Type::voidTy());
    const Type *callback = call->args[1]->type;
    codegen_.reactionLowerer().attachReaction(
        answer, done,
        codegen_.reactionLowerer().reactionHandler(Builtin::PromiseThen, Type::string(), Type::voidTy(), callback, nullptr),
        values[1], nullptr);
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {done});
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {answer});
    codegen_.refCounts().releaseValue(values[0], call->args[0]->type);
    break;
  }
  case Builtin::RlClose:
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_readline_close", voidTy, {ptr}), {rl.value});
    break;
  default:
    llvm_unreachable("not a built-in of node:readline");
  }
  codegen_.refCounts().dropOperand(rl);

  return result;
}

// node:timers/promises.
llvm::Value *FsPromisesLowerer::emitAsyncIoBuiltin(CallExpr *call) {
  llvm::Type *voidTy = codegen_.builder_.getVoidTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Type *f64 = codegen_.builder_.getDoubleTy(), *ptr = codegen_.builder_.getPtrTy();
  switch (call->builtin) {
  case Builtin::TimersSetTimeout:
  case Builtin::TimersSetImmediate:
  case Builtin::TimersSetInterval: {
    // [delay], [value], signal, ref.
    std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
    bool immediate = call->builtin == Builtin::TimersSetImmediate;
    llvm::Value *delay = immediate ? llvm::ConstantFP::get(f64, 0.0) : values[0];
    size_t at = immediate ? 0 : 1;
    const Type *value = call->builtin == Builtin::TimersSetInterval ? call->type->yielded()
                                                                    : call->type->promised();
    bool holds = call->args.size() - at == 3;
    llvm::Value *given = holds ? values[at] : nullptr;
    llvm::Value *signal = values[values.size() - 2];
    llvm::Value *ref = codegen_.builder_.CreateZExt(values.back(), i32);
    llvm::Value *result;
    if (call->builtin == Builtin::TimersSetInterval) {
      // The iterator keeps the value, which it copies into each step.
      llvm::Value *slot = given ? codegen_.functionFrame().spill(given, "interval.value")
                                : llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
      llvm::Value *type = codegen_.promiseLowerer().promiseValueSize(value)
                              ? static_cast<llvm::Value *>(codegen_.typeDescriptors().typeDescriptor(value))
                              : llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
      result =
          codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_timers_interval", ptr, {f64, ptr, ptr, ptr, i32}),
                              {delay, slot, type, signal, ref}, "interval");
    } else {
      // The promise holds the value until the time is up.
      result = codegen_.promiseLowerer().newPromise(value);
      if (given && codegen_.promiseLowerer().promiseValueSize(value)) {
        codegen_.builder_.CreateStore(given, codegen_.promiseLowerer().promiseValueAddress(result));
      }
      codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_timers_wait", voidTy, {ptr, f64, i32, ptr, i32}),
                          {result, delay, codegen_.builder_.getInt32(immediate), signal, ref});
    }
    // The signal is borrowed.
    codegen_.refCounts().releaseValue(signal, call->args[call->args.size() - 2]->type);
    return result;
  }
  default:
    llvm_unreachable("not a built-in of waiting for I/O");
  }
}

} // namespace tz
