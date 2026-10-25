#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

FunctionBuilder::FunctionBuilder(CodeGen &codegen) : codegen_(codegen) {}

void FunctionBuilder::setFunctionAttributes(llvm::Function *fn) {
  // Nothing unwinds yet, but unwind tables still let debuggers and crash
  // handlers walk the stack.
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  fn->setUWTableKind(llvm::UWTableKind::Default);
}

void FunctionBuilder::declareFunction(FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;
  std::vector<llvm::Type *> params;
  for (const Type *param : sym->params) {
    params.push_back(codegen_.moduleBuilder().lower(param));
  }
  auto *type = llvm::FunctionType::get(codegen_.moduleBuilder().lower(sym->ret), params, false);
  // The whole program is one module, so everything but `main` is internal and
  // LLVM is free to inline or drop it. An instance of a generic function is
  // named with its type arguments, as Sema made them (see
  // Sema::representation): `tz.first<int>`, `tz.first<object>`.
  std::string name = "tz." + codegen_.symbolPrefixes_.lookup(fn) + fn->name.str();
  if (fn->cls) { // a static method
    name = "tz." + codegen_.symbolPrefixes_.lookup(fn->cls) + fn->cls->name.str() + "." + fn->name.str() +
           (fn->variant ? "." + std::to_string(fn->variant) : "");
  }
  if (fn->isInstance) {
    name += "<";
    for (size_t i = 0; i < fn->typeArgs.size(); ++i) {
      const Type *arg = fn->typeArgs[i];
      name +=
          (i > 0 ? ", " : "") + (arg->isFunction() ? std::string("function") : arg->name().str());
    }
    name += ">";
  }
  auto *function =
      llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, codegen_.module_.get());
  setFunctionAttributes(function);
  codegen_.functions_[sym] = function;
}

void FunctionBuilder::beginFunction(llvm::Function *function, FunctionDecl *fn) {
  codegen_.function_ = function;
  codegen_.currentFn_ = fn;
  codegen_.returnType_ = fn ? fn->sym->ret : nullptr;
  codegen_.envParam_ = nullptr;
  codegen_.unwindExit_ = nullptr;
  codegen_.addresses_.clear();
  codegen_.boxSlots_.clear();
  codegen_.envBoxes_.clear();
  codegen_.groupEnvs_.clear();
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", codegen_.function_));
}

void FunctionBuilder::emitFunction(FunctionDecl *fn) {
  if (fn->external) {
    codegen_.tzModulesLowerer().emitExternal(fn);
    return;
  }
  if (fn->impl) { // a static method a decorator may replace
    codegen_.decoratorLowerer().emitDecoratedMethod(fn);
    return;
  }
  if (fn->isGenerator) {
    codegen_.generatorLowerer().emitGenerator(fn, codegen_.functions_.lookup(fn->sym));
    return;
  }
  if (fn->isAsync) {
    codegen_.asyncLowerer().emitAsyncFunction(fn, codegen_.functions_.lookup(fn->sym));
    return;
  }
  if (CodeGen::TailGroup *group = codegen_.tailGroupOf_.lookup(fn)) {
    codegen_.tailCallLowerer().emitTailGroup(*group);
    return;
  }
  beginFunction(codegen_.functions_.lookup(fn->sym), fn);
  std::vector<llvm::Value *> args;
  for (llvm::Argument &arg : codegen_.function_->args()) {
    args.push_back(&arg);
  }
  emitParamsAndBody(fn, args);
  finishFunction();
}

// The parameters, from the arguments given, and the body.
void FunctionBuilder::emitParamsAndBody(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  codegen_.functionFrame().pushScope(); // parameters: the callee owns its arguments
  emitParams(fn, args);
  // An owner its environment refers to without counting (layer 2 of
  // docs/objects.md, section 11) stays while the closure runs, even if what
  // it runs lets the owner go.
  if (fn && fn->isClosure() && !fn->group) {
    for (const VarSymbol *var : fn->captures) {
      if (codegen_.uncounted_.count({fn, var})) {
        const Type *type = ClosureLowerer::typeOf(var);
        llvm::Value *owner = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), codegen_.functionFrame().addressOf(var), var->name);
        codegen_.refCounts().retainValue(owner, type);
        codegen_.builder_.CreateStore(owner, codegen_.functionFrame().createManagedSlot(type, var->name + ".held"));
      }
    }
  }
  // A function that may recurse checks the stack first (docs/errors.md).
  if (codegen_.recursive_.contains(fn)) {
    codegen_.throwLowerer().emitStackCheck(fn->loc());
  }
  codegen_.stmtLowerer().emitStmt(fn->body);
  codegen_.functionFrame().popScope();
}

void FunctionBuilder::emitParams(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  if (fn->thisVar) {
    // A method's object, the environment it gets: borrowed, as the caller
    // keeps it alive during the call.
    llvm::Value *slot = codegen_.functionFrame().createStackSlot(codegen_.builder_.getPtrTy(), "this");
    codegen_.builder_.CreateStore(codegen_.envParam_, slot);
    codegen_.addresses_[fn->thisVar] = slot;
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    llvm::Value *arg = args[i];
    if (isa<llvm::Argument>(arg)) {
      arg->setName(param.name);
    }
    // Parameters can be assigned to, so they live in stack slots like locals.
    llvm::Value *slot = codegen_.functionFrame().createLocal(param.sym, param.name);
    if (!param.defaultValue) {
      codegen_.storeLowerer().storeVar(param.sym, slot, arg, /*replacing=*/false);
      continue;
    }
    // The argument is `T | undefined`; undefined takes the default value.
    const Type *passed = Type::optional(param.sym->type);
    llvm::BasicBlock *givenBlock = codegen_.functionFrame().newBlock("param.given");
    llvm::BasicBlock *defaultBlock = codegen_.functionFrame().newBlock("param.default");
    llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("param.done");
    codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(arg, passed), givenBlock, defaultBlock);
    codegen_.functionFrame().switchTo(givenBlock);
    llvm::Value *given = param.sym->type->isOptional() ? arg : codegen_.optionalLowerer().optionalValue(arg, passed);
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(defaultBlock);
    llvm::Value *fallback = codegen_.exprLowerer().emitExpr(param.defaultValue);
    llvm::BasicBlock *defaultEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(doneBlock);
    codegen_.functionFrame().switchTo(doneBlock);
    llvm::PHINode *value = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(param.sym->type), 2);
    value->addIncoming(given, givenBlock);
    value->addIncoming(fallback, defaultEnd);
    codegen_.storeLowerer().storeVar(param.sym, slot, value, /*replacing=*/false);
  }
}

void FunctionBuilder::finishFunction() {
  closeBody();
  codegen_.unwindLowerer().finishFunctionUnwind();
  codegen_.function_ = nullptr;
}

// Control that reaches the end of a body returns.
void FunctionBuilder::closeBody() {
  if (codegen_.functionFrame().blockIsOpen()) {
    llvm::Type *ret = codegen_.function_->getReturnType();
    if (ret->isVoidTy()) {
      codegen_.builder_.CreateRetVoid();
    } else if (codegen_.function_->hasExternalLinkage()) { // main
      codegen_.builder_.CreateRet(codegen_.builder_.getInt32(0));
    } else if (codegen_.returnType_ &&
               (codegen_.returnType_->isOptional() || codegen_.returnType_->isUnknown())) { // undefined
      codegen_.builder_.CreateRet(codegen_.optionalLowerer().undefinedValue(codegen_.returnType_));
    } else { // Sema proved that every path returns
      codegen_.builder_.CreateUnreachable();
    }
  }
}

} // namespace tz
