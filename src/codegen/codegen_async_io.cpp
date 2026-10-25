// Waiting for I/O (docs/async.md, step 2): calls of runtime/fs.c for
// node:fs/promises and node:readline, of runtime/async.c for
// node:timers/promises; what they take of an AbortSignal is its side of the
// runtime (runtime/signal.c).
//
// A function that gives a Promise makes it here, of the type Sema gave, and
// the runtime settles it later: it holds the promise meanwhile and writes the
// value into it. The arguments are borrowed, as for the functions of node:fs
// (codegen_node.cpp), and an error in them rejects the promise rather than
// being thrown.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;

namespace tz {

llvm::Value *CodeGen::emitFsPromise(CallExpr *call) {
  llvm::Type *voidTy = builder_.getVoidTy(), *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty(), *ptr = builder_.getPtrTy();
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  const Type *value = call->type->promised();
  llvm::Value *promise = newPromise(value);
  auto obj = [&](size_t i) { return stringObj(values[i]); };
  auto meta = [&](size_t i) { return stringMeta(values[i]); };
  auto flag = [&](size_t i) { return builder_.CreateZExt(values[i], i32); };
  // The runtime function, with the promise first and the maker of
  // SystemError last.
  auto run = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                 std::vector<llvm::Value *> args) {
    params.insert(params.begin(), ptr);
    args.insert(args.begin(), promise);
    params.push_back(ptr);
    args.push_back(systemErrorMaker());
    builder_.CreateCall(processFunction(name, voidTy, params), args);
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
    if (!call->args[1]->type->isString())
      run("tz_fsp_write_file_bytes", {ptr, i64, ptr, ptr, i64, i32, ptr},
          {obj(0), meta(0), values[1], obj(2), meta(2), values[3], values[4]});
    else
      run("tz_fsp_write_file", {ptr, i64, ptr, i64, ptr, i64, i32, ptr, i32},
          {obj(0), meta(0), obj(1), meta(1), obj(2), meta(2), values[3], values[5], values[4]});
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
    run("tz_fsp_stat", {ptr, i64, i32, ptr}, {obj(0), meta(0), flag(1), objectMaker(value)});
    break;
  case Builtin::FspReaddir: {
    const Type *element = value->inner();
    llvm::Value *dirent = element->isClass() ? static_cast<llvm::Value *>(objectMaker(element))
                                             : llvm::ConstantPointerNull::get(builder_.getPtrTy());
    run("tz_fsp_readdir", {ptr, i64, i32, ptr, ptr},
        {obj(0), meta(0), flag(1), typeDescriptor(element), dirent});
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
        {obj(0), meta(0), builder_.getInt32(call->builtin == Builtin::FspUnlink)});
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
  for (size_t i = 0; i < values.size(); ++i)
    releaseValue(values[i], call->args[i]->type);
  return promise;
}

llvm::Value *CodeGen::emitReadlineBuiltin(CallExpr *call) {
  llvm::Type *voidTy = builder_.getVoidTy(), *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty(), *ptr = builder_.getPtrTy();
  if (call->builtin == Builtin::RlCreate) {
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    llvm::Value *rl = builder_.CreateCall(processFunction("tz_readline_new", ptr, {i32, ptr}),
                                          {values[0], values[1]}, "readline");
    releaseValue(values[1], call->args[1]->type);
    return rl;
  }
  Operand rl = emitOperand(cast<MemberExpr>(call->callee)->object, /*canBorrow=*/true);
  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::RlQuestion: {
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    result = newPromise(Type::string());
    builder_.CreateCall(processFunction("tz_readline_question", voidTy, {ptr, ptr, ptr, i64, ptr}),
                        {rl.value, result, stringObj(values[0]), stringMeta(values[0]), values[1]});
    for (size_t i = 0; i < values.size(); ++i)
      releaseValue(values[i], call->args[i]->type);
    break;
  }
  case Builtin::RlQuestionCallback: {
    // The question of node:readline/promises, whose answer goes to the
    // callback as then() would give it; an error in it, or a question of an
    // interface closed already, ends the program.
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    llvm::Value *answer = newPromise(Type::string());
    builder_.CreateCall(processFunction("tz_readline_question", voidTy, {ptr, ptr, ptr, i64, ptr}),
                        {rl.value, answer, stringObj(values[0]), stringMeta(values[0]),
                         llvm::ConstantPointerNull::get(builder_.getPtrTy())});
    llvm::Value *done = newPromise(Type::voidTy());
    const Type *callback = call->args[1]->type;
    attachReaction(answer, done,
                   reactionHandler(Builtin::PromiseThen, Type::string(), Type::voidTy(), callback,
                                   nullptr),
                   values[1], nullptr);
    builder_.CreateCall(instanceReleaseFn_, {done});
    builder_.CreateCall(instanceReleaseFn_, {answer});
    releaseValue(values[0], call->args[0]->type);
    break;
  }
  case Builtin::RlClose:
    builder_.CreateCall(processFunction("tz_readline_close", voidTy, {ptr}), {rl.value});
    break;
  default:
    llvm_unreachable("not a built-in of node:readline");
  }
  dropOperand(rl);
  return result;
}

// node:timers/promises.
llvm::Value *CodeGen::emitAsyncIoBuiltin(CallExpr *call) {
  llvm::Type *voidTy = builder_.getVoidTy(), *i32 = builder_.getInt32Ty();
  llvm::Type *f64 = builder_.getDoubleTy(), *ptr = builder_.getPtrTy();
  switch (call->builtin) {
  case Builtin::TimersSetTimeout:
  case Builtin::TimersSetImmediate:
  case Builtin::TimersSetInterval: {
    // [delay], [value], signal, ref.
    std::vector<llvm::Value *> values = emitHeldValues(call->args);
    bool immediate = call->builtin == Builtin::TimersSetImmediate;
    llvm::Value *delay = immediate ? llvm::ConstantFP::get(f64, 0.0) : values[0];
    size_t at = immediate ? 0 : 1;
    const Type *value = call->builtin == Builtin::TimersSetInterval ? call->type->yielded()
                                                                    : call->type->promised();
    bool holds = call->args.size() - at == 3;
    llvm::Value *given = holds ? values[at] : nullptr;
    llvm::Value *signal = values[values.size() - 2];
    llvm::Value *ref = builder_.CreateZExt(values.back(), i32);
    llvm::Value *result;
    if (call->builtin == Builtin::TimersSetInterval) {
      // The iterator keeps the value, which it copies into each step.
      llvm::Value *slot = given ? spill(given, "interval.value")
                                : llvm::ConstantPointerNull::get(builder_.getPtrTy());
      llvm::Value *type = promiseValueSize(value)
                              ? static_cast<llvm::Value *>(typeDescriptor(value))
                              : llvm::ConstantPointerNull::get(builder_.getPtrTy());
      result = builder_.CreateCall(
          processFunction("tz_timers_interval", ptr, {f64, ptr, ptr, ptr, i32}),
          {delay, slot, type, signal, ref}, "interval");
    } else {
      // The promise holds the value until the time is up.
      result = newPromise(value);
      if (given && promiseValueSize(value))
        builder_.CreateStore(given, promiseValueAddress(result));
      builder_.CreateCall(processFunction("tz_timers_wait", voidTy, {ptr, f64, i32, ptr, i32}),
                          {result, delay, builder_.getInt32(immediate), signal, ref});
    }
    // The signal is borrowed.
    releaseValue(signal, call->args[call->args.size() - 2]->type);
    return result;
  }
  default:
    llvm_unreachable("not a built-in of waiting for I/O");
  }
}

} // namespace tz
