#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

ProcessLowerer::ProcessLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::FunctionCallee ProcessLowerer::processFunction(llvm::StringRef name, llvm::Type *ret,
                                              llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      codegen_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *ProcessLowerer::emitProcessBuiltin(CallExpr *call) {
  llvm::Type *voidTy = codegen_.builder_.getVoidTy();
  llvm::Type *i32 = codegen_.builder_.getInt32Ty();
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();
  llvm::Type *f64 = codegen_.builder_.getDoubleTy();
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *zero = codegen_.builder_.getInt32(0);
  // A string that the runtime returns through a pointer.
  auto stringResult = [&](llvm::StringRef name) {
    return codegen_.stringLowerer().callReturningString(processFunction(name, voidTy, {ptr}), {});
  };
  // An exit code, `int | undefined`, as the runtime takes it: the code and
  // whether there is one.
  auto exitCode = [&]() -> std::pair<llvm::Value *, llvm::Value *> {
    if (call->args.empty()) {
      return {zero, zero};
    }
    llvm::Value *code = codegen_.exprLowerer().emitExpr(call->args[0]);

    return {codegen_.builder_.CreateExtractValue(code, 0),
            codegen_.builder_.CreateZExt(codegen_.builder_.CreateExtractValue(code, 1), i32)};
  };

  switch (call->builtin) {
  case Builtin::ProcessArgv:
    // One array that the runtime keeps; this reference is the caller's.
    return codegen_.builder_.CreateCall(processFunction("tz_process_argv", ptr, {ptr}),
                               {codegen_.typeDescriptors().typeDescriptor(Type::string())});
  case Builtin::ProcessEnv: {
    CodeGen::Operand name = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::AllocaInst *value = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "env.value");
    llvm::Value *found =
        codegen_.builder_.CreateCall(processFunction("tz_process_env", i32, {ptr, ptr, i64}),
                            {value, codegen_.stringLowerer().stringObj(name.value), codegen_.stringLowerer().stringMeta(name.value)});
    codegen_.refCounts().dropOperand(name);
    return codegen_.optionalLowerer().makeOptional(codegen_.builder_.CreateLoad(codegen_.strTy_, value), codegen_.builder_.CreateICmpNE(found, zero),
                        call->type);
  }
  case Builtin::ProcessExit: {
    auto [code, has] = exitCode();
    llvm::FunctionCallee exit = processFunction("tz_process_exit", voidTy, {i32, i32});
    cast<llvm::Function>(exit.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
    codegen_.builder_.CreateCall(exit, {code, has})->setDoesNotReturn();
    codegen_.builder_.CreateUnreachable();
    codegen_.functionFrame().startDeadBlock();
    return nullptr;
  }
  case Builtin::ProcessExitCode: {
    llvm::AllocaInst *has = codegen_.functionFrame().createStackSlot(i32, "exit.has");
    llvm::Value *code =
        codegen_.builder_.CreateCall(processFunction("tz_process_get_exit_code", i32, {ptr}), {has});
    return codegen_.optionalLowerer().makeOptional(code, codegen_.builder_.CreateICmpNE(codegen_.builder_.CreateLoad(i32, has), zero),
                        call->type);
  }
  case Builtin::ProcessSetExitCode: {
    auto [code, has] = exitCode();
    codegen_.builder_.CreateCall(processFunction("tz_process_set_exit_code", voidTy, {i32, i32}),
                        {code, has});
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
      CodeGen::Operand bytes = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
      codegen_.builder_.CreateCall(
          processFunction("tz_lib_std_write", codegen_.builder_.getVoidTy(), {i32, codegen_.builder_.getPtrTy()}),
          {codegen_.builder_.getInt32(toStdout ? 1 : 2), bytes.value});
      codegen_.refCounts().dropOperand(bytes);
      return codegen_.builder_.getTrue();
    }
    CodeGen::Operand text = codegen_.refCounts().emitOperand(call->args[0], /*canBorrow=*/true);
    llvm::Value *written = codegen_.builder_.CreateCall(
        processFunction(toStdout ? "tz_stdout_write" : "tz_stderr_write", i32, {ptr, i64}),
        {codegen_.stringLowerer().stringObj(text.value), codegen_.stringLowerer().stringMeta(text.value)});
    codegen_.refCounts().dropOperand(text);
    return codegen_.builder_.CreateICmpNE(written, zero);
  }
  case Builtin::ProcessIsTTY:
    return codegen_.builder_.CreateICmpNE(
        codegen_.builder_.CreateCall(processFunction("tz_is_tty", i32, {i32}), {codegen_.exprLowerer().emitExpr(call->args[0])}),
        zero);
  case Builtin::DateNow:
    return codegen_.builder_.CreateCall(processFunction("tz_date_now", f64, {}));
  case Builtin::PerformanceNow:
    return codegen_.builder_.CreateCall(processFunction("tz_performance_now", f64, {}));
  default:
    llvm_unreachable("not a built-in of process");
  }
}

} // namespace tz
