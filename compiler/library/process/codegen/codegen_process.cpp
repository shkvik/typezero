// process, Date.now() and performance.now(): calls of
// runtime/library/process/process/process.c, and of the output functions of
// runtime/core/tzrt/tzrt.c for process.stdout.write and the like.

#include "compiler/lower/codegen/codegen.h"

#include "llvm/Support/ErrorHandling.h"

using llvm::cast;

namespace tz {

llvm::FunctionCallee CodeGen::processFunction(llvm::StringRef name, llvm::Type *ret,
                                              llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
  return callee;
}

llvm::Value *CodeGen::emitProcessBuiltin(CallExpr *call) {
  llvm::Type *voidTy = builder_.getVoidTy();
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty();
  llvm::Type *f64 = builder_.getDoubleTy();
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *zero = builder_.getInt32(0);
  // A string that the runtime returns through a pointer.
  auto stringResult = [&](llvm::StringRef name) {
    return callReturningString(processFunction(name, voidTy, {ptr}), {});
  };
  // An exit code, `int | undefined`, as the runtime takes it: the code and
  // whether there is one.
  auto exitCode = [&]() -> std::pair<llvm::Value *, llvm::Value *> {
    if (call->args.empty())
      return {zero, zero};
    llvm::Value *code = emitExpr(call->args[0]);
    return {builder_.CreateExtractValue(code, 0),
            builder_.CreateZExt(builder_.CreateExtractValue(code, 1), i32)};
  };

  switch (call->builtin) {
  case Builtin::ProcessArgv:
    // One array that the runtime keeps; this reference is the caller's.
    return builder_.CreateCall(processFunction("tz_process_argv", ptr, {ptr}),
                               {typeDescriptor(Type::string())});
  case Builtin::ProcessEnv: {
    Operand name = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::AllocaInst *value = createStackSlot(strTy_, "env.value");
    llvm::Value *found = builder_.CreateCall(processFunction("tz_process_env", i32, {ptr, ptr, i64}),
                                             {value, stringObj(name.value), stringMeta(name.value)});
    dropOperand(name);
    return makeOptional(builder_.CreateLoad(strTy_, value), builder_.CreateICmpNE(found, zero),
                        call->type);
  }
  case Builtin::ProcessExit: {
    auto [code, has] = exitCode();
    llvm::FunctionCallee exit = processFunction("tz_process_exit", voidTy, {i32, i32});
    cast<llvm::Function>(exit.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
    builder_.CreateCall(exit, {code, has})->setDoesNotReturn();
    builder_.CreateUnreachable();
    startDeadBlock();
    return nullptr;
  }
  case Builtin::ProcessExitCode: {
    llvm::AllocaInst *has = createStackSlot(i32, "exit.has");
    llvm::Value *code =
        builder_.CreateCall(processFunction("tz_process_get_exit_code", i32, {ptr}), {has});
    return makeOptional(code, builder_.CreateICmpNE(builder_.CreateLoad(i32, has), zero), call->type);
  }
  case Builtin::ProcessSetExitCode: {
    auto [code, has] = exitCode();
    builder_.CreateCall(processFunction("tz_process_set_exit_code", voidTy, {i32, i32}), {code, has});
    return nullptr;
  }
  case Builtin::ProcessCwd:
    return stringResult("tz_process_cwd");
  case Builtin::ProcessExecPath:
    return stringResult("tz_process_exec_path");
  case Builtin::ProcessPlatform:
    return stringResult("tz_process_platform");
  case Builtin::ProcessStdoutWrite:
  case Builtin::ProcessStderrWrite: {
    bool toStdout = call->builtin == Builtin::ProcessStdoutWrite;
    if (const Type *type = call->args[0]->type; type->isBuffer() || type->isBytes()) {
      // The bytes of a Buffer or a Uint8Array, as they are
      // (runtime/library/stream/stream/stream.c).
      Operand bytes = emitOperand(call->args[0], /*canBorrow=*/true);
      builder_.CreateCall(processFunction("tz_lib_std_write", builder_.getVoidTy(),
                                          {i32, builder_.getPtrTy()}),
                          {builder_.getInt32(toStdout ? 1 : 2), bytes.value});
      dropOperand(bytes);
      return builder_.getTrue();
    }
    Operand text = emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *written =
        builder_.CreateCall(processFunction(toStdout ? "tz_stdout_write" : "tz_stderr_write", i32, {ptr, i64}),
                            {stringObj(text.value), stringMeta(text.value)});
    dropOperand(text);
    return builder_.CreateICmpNE(written, zero);
  }
  case Builtin::ProcessIsTTY:
    return builder_.CreateICmpNE(
        builder_.CreateCall(processFunction("tz_is_tty", i32, {i32}), {emitExpr(call->args[0])}), zero);
  case Builtin::DateNow:
    return builder_.CreateCall(processFunction("tz_date_now", f64, {}));
  case Builtin::PerformanceNow:
    return builder_.CreateCall(processFunction("tz_performance_now", f64, {}));
  default:
    llvm_unreachable("not a built-in of process");
  }
}

} // namespace tz
