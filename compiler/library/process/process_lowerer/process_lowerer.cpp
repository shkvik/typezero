#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

ProcessLowerer::ProcessLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::FunctionCallee ProcessLowerer::processFunction(llvm::StringRef name, llvm::Type *ret,
                                                     llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      lowering_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

void ProcessLowerer::emitPanic(llvm::StringRef message) {
  llvm::Value *text = lowering_.stringLowerer().utf8Constant(message.str());
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i64 = lowering_.builder_.getInt64Ty();
  auto *fn = cast<llvm::Function>(
      processFunction("tz_panic", lowering_.builder_.getVoidTy(), {ptr, i64}).getCallee());
  fn->addFnAttr(llvm::Attribute::Cold);
  fn->addFnAttr(llvm::Attribute::NoReturn);
  lowering_.builder_.CreateCall(fn, {text, lowering_.builder_.getInt64(message.size())});
  lowering_.builder_.CreateUnreachable();
}

llvm::Value *ProcessLowerer::emitProcessBuiltin(CallExpr *call) {
  llvm::Type *voidTy = lowering_.builder_.getVoidTy();
  llvm::Type *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  llvm::Type *f64 = lowering_.builder_.getDoubleTy();
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *zero = lowering_.builder_.getInt32(0);
  auto stringResult = [&](llvm::StringRef name) {
    return lowering_.stringLowerer().callReturningString(processFunction(name, voidTy, {ptr}), {});
  };
  auto exitCode = [&]() -> std::pair<llvm::Value *, llvm::Value *> {
    if (call->args.empty()) {
      return {zero, zero};
    }
    llvm::Value *code = lowering_.exprLowerer().emitExpr(call->args[0]);

    return {lowering_.builder_.CreateExtractValue(code, 0),
            lowering_.builder_.CreateZExt(lowering_.builder_.CreateExtractValue(code, 1), i32)};
  };

  switch (call->builtin) {
  case Builtin::ProcessArgv:
    return lowering_.builder_.CreateCall(
        processFunction("tz_process_argv", ptr, {ptr}),
        {lowering_.typeDescriptors().typeDescriptor(Type::string())});
  case Builtin::ProcessEnv: {
    Lowering::Operand name = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::AllocaInst *value =
        lowering_.functionFrame().createStackSlot(lowering_.strTy_, "env.value");
    llvm::Value *found =
        lowering_.builder_.CreateCall(processFunction("tz_process_env", i32, {ptr, ptr, i64}),
                                      {value, lowering_.stringLowerer().stringObj(name.value),
                                       lowering_.stringLowerer().stringMeta(name.value)});
    lowering_.refCounts().dropOperand(name);
    return lowering_.optionalLowerer().makeOptional(
        lowering_.builder_.CreateLoad(lowering_.strTy_, value),
        lowering_.builder_.CreateICmpNE(found, zero), call->type);
  }
  case Builtin::ProcessExit: {
    auto [code, has] = exitCode();
    llvm::FunctionCallee exit = processFunction("tz_process_exit", voidTy, {i32, i32});
    cast<llvm::Function>(exit.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
    lowering_.builder_.CreateCall(exit, {code, has})->setDoesNotReturn();
    lowering_.builder_.CreateUnreachable();
    lowering_.functionFrame().startDeadBlock();
    return nullptr;
  }
  case Builtin::ProcessExitCode: {
    llvm::AllocaInst *has = lowering_.functionFrame().createStackSlot(i32, "exit.has");
    llvm::Value *code = lowering_.builder_.CreateCall(
        processFunction("tz_process_get_exit_code", i32, {ptr}), {has});
    return lowering_.optionalLowerer().makeOptional(
        code, lowering_.builder_.CreateICmpNE(lowering_.builder_.CreateLoad(i32, has), zero),
        call->type);
  }
  case Builtin::ProcessSetExitCode: {
    auto [code, has] = exitCode();
    lowering_.builder_.CreateCall(processFunction("tz_process_set_exit_code", voidTy, {i32, i32}),
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
      Lowering::Operand bytes = lowering_.refCounts().emitOperand(call->args[0], true);
      lowering_.builder_.CreateCall(processFunction("tz_lib_std_write",
                                                    lowering_.builder_.getVoidTy(),
                                                    {i32, lowering_.builder_.getPtrTy()}),
                                    {lowering_.builder_.getInt32(toStdout ? 1 : 2), bytes.value});
      lowering_.refCounts().dropOperand(bytes);
      return lowering_.builder_.getTrue();
    }
    Lowering::Operand text = lowering_.refCounts().emitOperand(call->args[0], true);
    llvm::Value *written = lowering_.builder_.CreateCall(
        processFunction(toStdout ? "tz_stdout_write" : "tz_stderr_write", i32, {ptr, i64}),
        {lowering_.stringLowerer().stringObj(text.value),
         lowering_.stringLowerer().stringMeta(text.value)});
    lowering_.refCounts().dropOperand(text);
    return lowering_.builder_.CreateICmpNE(written, zero);
  }
  case Builtin::ProcessIsTTY:
    return lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateCall(processFunction("tz_is_tty", i32, {i32}),
                                      {lowering_.exprLowerer().emitExpr(call->args[0])}),
        zero);
  case Builtin::DateNow:
    return lowering_.builder_.CreateCall(processFunction("tz_date_now", f64, {}));
  case Builtin::PerformanceNow:
    return lowering_.builder_.CreateCall(processFunction("tz_performance_now", f64, {}));
  default:
    llvm_unreachable("not a built-in of process");
  }
}

}
