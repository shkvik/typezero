#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;

namespace tz {

ProcessLowerer::ProcessLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::FunctionCallee ProcessLowerer::processFunction(llvm::StringRef name, llvm::Type *ret,
                                                     llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee = this->lowering.module_->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

void ProcessLowerer::emitPanic(llvm::StringRef message) {
  llvm::Value *text = this->lowering.stringLowerer().utf8Constant(message.str());
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  auto *fn = cast<llvm::Function>(
      this->processFunction("tz_panic", this->lowering.builder.getVoidTy(), {ptr, i64})
          .getCallee());
  fn->addFnAttr(llvm::Attribute::Cold);
  fn->addFnAttr(llvm::Attribute::NoReturn);
  this->lowering.builder.CreateCall(fn, {text, this->lowering.builder.getInt64(message.size())});
  this->lowering.builder.CreateUnreachable();
}

llvm::Value *ProcessLowerer::emitProcessBuiltin(CallExpr *call) {
  llvm::Type *voidTy = this->lowering.builder.getVoidTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Value *zero = this->lowering.builder.getInt32(0);
  auto stringResult = [&](llvm::StringRef name) {
    return this->lowering.stringLowerer().callReturningString(
        this->processFunction(name, voidTy, {ptr}), {});
  };
  auto exitCode = [&]() -> std::pair<llvm::Value *, llvm::Value *> {
    if (call->args.empty()) {
      return {this->lowering.builder.getInt64(0), zero};
    }
    llvm::Value *code = this->lowering.exprLowerer().emitExpr(call->args[0]);

    return {
        this->lowering.builder.CreateExtractValue(code, 0),
        this->lowering.builder.CreateZExt(this->lowering.builder.CreateExtractValue(code, 1), i32)};
  };

  switch (call->builtin()) {
    case Builtin::ProcessArgv:
      return this->lowering.builder.CreateCall(
          this->processFunction("tz_process_argv", ptr, {ptr}),
          {this->lowering.typeDescriptors().typeDescriptor(Type::string())});
    case Builtin::ProcessEnv: {
      Lowering::Operand name = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::AllocaInst *value =
          this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "env.value");
      llvm::Value *found = this->lowering.builder.CreateCall(
          this->processFunction("tz_process_env", i32, {ptr, ptr, i64}),
          {value, this->lowering.stringLowerer().stringObj(name.value),
           this->lowering.stringLowerer().stringMeta(name.value)});
      this->lowering.refCounts().dropOperand(name);
      return this->lowering.optionalLowerer().makeOptional(
          this->lowering.builder.CreateLoad(this->lowering.strTy, value),
          this->lowering.builder.CreateICmpNE(found, zero), call->type);
    }
    case Builtin::ProcessExit: {
      auto [code, has] = exitCode();
      llvm::FunctionCallee exit = this->processFunction("tz_process_exit", voidTy, {i64, i32});
      cast<llvm::Function>(exit.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
      this->lowering.builder.CreateCall(exit, {code, has})->setDoesNotReturn();
      this->lowering.builder.CreateUnreachable();
      this->lowering.functionFrame().startDeadBlock();
      return nullptr;
    }
    case Builtin::ProcessExitCode: {
      llvm::AllocaInst *has = this->lowering.functionFrame().createStackSlot(i32, "exit.has");
      llvm::Value *code = this->lowering.builder.CreateSExt(
          this->lowering.builder.CreateCall(
              this->processFunction("tz_process_get_exit_code", i32, {ptr}), {has}),
          this->lowering.builder.getInt64Ty());
      return this->lowering.optionalLowerer().makeOptional(
          code,
          this->lowering.builder.CreateICmpNE(this->lowering.builder.CreateLoad(i32, has), zero),
          call->type);
    }
    case Builtin::ProcessSetExitCode: {
      auto [code, has] = exitCode();
      this->lowering.builder.CreateCall(
          this->processFunction("tz_process_set_exit_code", voidTy, {i64, i32}), {code, has});
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
      bool toStdout = call->builtin() == Builtin::ProcessStdoutWrite;
      if (const Type *type = call->args[0]->type; type->isBuffer() || type->isBytes()) {
        Lowering::Operand bytes = this->lowering.refCounts().emitOperand(call->args[0], true);
        this->lowering.builder.CreateCall(
            this->processFunction(
                "tz_lib_std_write", this->lowering.builder.getVoidTy(),
                {this->lowering.builder.getInt64Ty(), this->lowering.builder.getPtrTy()}),
            {this->lowering.builder.getInt64(toStdout ? 1 : 2), bytes.value});
        this->lowering.refCounts().dropOperand(bytes);
        return this->lowering.builder.getTrue();
      }
      Lowering::Operand text = this->lowering.refCounts().emitOperand(call->args[0], true);
      llvm::Value *written = this->lowering.builder.CreateCall(
          this->processFunction(toStdout ? "tz_stdout_write" : "tz_stderr_write", i32, {ptr, i64}),
          {this->lowering.stringLowerer().stringObj(text.value),
           this->lowering.stringLowerer().stringMeta(text.value)});
      this->lowering.refCounts().dropOperand(text);
      return this->lowering.builder.CreateICmpNE(written, zero);
    }
    case Builtin::ProcessIsTTY:
      return this->lowering.builder.CreateICmpNE(
          this->lowering.builder.CreateCall(
              this->processFunction("tz_is_tty", i32, {this->lowering.builder.getInt64Ty()}),
              {this->lowering.exprLowerer().emitExpr(call->args[0])}),
          zero);
    case Builtin::DateNow:
      return this->lowering.builder.CreateCall(
          this->processFunction("tz_date_now", this->lowering.builder.getInt64Ty(), {}));
    case Builtin::PerformanceNow:
      return this->lowering.builder.CreateCall(
          this->processFunction("tz_performance_now", f64, {}));
    case Builtin::ProcessPid:
      return this->lowering.builder.CreateSExt(
          this->lowering.builder.CreateCall(this->processFunction("tz_process_pid", i32, {})), i64);
    case Builtin::ProcessUptime:
      return this->lowering.builder.CreateFDiv(
          this->lowering.builder.CreateCall(this->processFunction("tz_performance_now", f64, {})),
          llvm::ConstantFP::get(f64, 1000.0), "uptime");
    default:
      llvm_unreachable("not a built-in of process");
  }
}

}
