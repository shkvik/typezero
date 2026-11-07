#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/debug_info/debug_info.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"

using llvm::isa;

namespace tz {

FunctionBuilder::FunctionBuilder(Lowering &owner)
    : lowering(owner) {}

void FunctionBuilder::setFunctionAttributes(llvm::Function *fn) {
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  fn->setUWTableKind(llvm::UWTableKind::Default);
}

void FunctionBuilder::declareFunction(FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;
  std::vector<llvm::Type *> params;
  for (const Type *param : sym->params) {
    params.push_back(this->lowering.moduleBuilder().lower(param));
  }
  auto *type =
      llvm::FunctionType::get(this->lowering.moduleBuilder().lower(sym->ret), params, false);
  std::string name = "tz." + this->lowering.symbolPrefixes_.lookup(fn) + fn->name.str();
  if (fn->cls()) {
    name = "tz." + this->lowering.symbolPrefixes_.lookup(fn->cls()) + fn->cls()->name.str() + "." +
           fn->name.str() + (fn->variant() ? "." + std::to_string(fn->variant()) : "");
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
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name,
                                          this->lowering.module_.get());
  this->setFunctionAttributes(function);
  this->lowering.functions_[sym] = function;
}

void FunctionBuilder::beginFunction(llvm::Function *function, FunctionDecl *fn) {
  this->lowering.function_ = function;
  this->lowering.currentFn_ = fn;
  this->lowering.returnType_ = fn ? fn->sym->ret : nullptr;
  this->lowering.envParam_ = nullptr;
  this->lowering.addresses_.clear();
  this->lowering.boxSlots_.clear();
  this->lowering.envBoxes_.clear();
  this->lowering.groupEnvs_.clear();
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", this->lowering.function_));
  this->lowering.debugInfo().beginFunction(function, fn);
}

void FunctionBuilder::emitFunction(FunctionDecl *fn) {
  if (fn->external()) {
    this->lowering.tzModulesLowerer().emitExternal(fn);
    return;
  }
  if (fn->impl()) {
    this->lowering.decoratorLowerer().emitDecoratedMethod(fn);
    return;
  }
  if (fn->isGenerator) {
    this->lowering.generatorLowerer().emitGenerator(fn, this->lowering.functions_.lookup(fn->sym));
    return;
  }
  if (fn->isAsync) {
    this->lowering.asyncLowerer().emitAsyncFunction(fn, this->lowering.functions_.lookup(fn->sym));
    return;
  }
  Lowering::TailGroup *group = this->lowering.tailGroupOf_.lookup(fn);
  if (group) {
    this->lowering.tailCallLowerer().emitTailGroup(*group);
    return;
  }
  this->beginFunction(this->lowering.functions_.lookup(fn->sym), fn);
  std::vector<llvm::Value *> args;
  for (llvm::Argument &arg : this->lowering.function_->args()) {
    args.push_back(&arg);
  }
  this->emitParamsAndBody(fn, args);
  this->finishFunction();
}

void FunctionBuilder::emitParamsAndBody(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  this->lowering.functionFrame().pushScope();
  this->emitParams(fn, args);
  if (fn && fn->isClosure() && !fn->group) {
    for (const VarSymbol *var : fn->captures) {
      if (this->lowering.memory().uncounted.count({fn, var})) {
        const Type *type = ClosureLowerer::typeOf(var);
        llvm::Value *owner = this->lowering.builder_.CreateLoad(
            this->lowering.moduleBuilder().lower(type),
            this->lowering.functionFrame().addressOf(var), var->name);
        this->lowering.refCounts().retainValue(owner, type);
        this->lowering.builder_.CreateStore(
            owner, this->lowering.functionFrame().createManagedSlot(type, var->name + ".held"));
      }
    }
  }
  this->lowering.stmtLowerer().emitStmt(fn->body);
  this->lowering.functionFrame().popScope();
}

void FunctionBuilder::emitParams(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  if (fn->thisVar()) {
    llvm::Value *slot =
        this->lowering.functionFrame().createStackSlot(this->lowering.builder_.getPtrTy(), "this");
    this->lowering.builder_.CreateStore(this->lowering.envParam_, slot);
    this->lowering.addresses_[fn->thisVar()] = slot;
    this->lowering.debugInfo().declare(fn->thisVar(), slot);
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    llvm::Value *arg = args[i];
    if (isa<llvm::Argument>(arg)) {
      arg->setName(param.name);
    }
    llvm::Value *slot = this->lowering.functionFrame().createLocal(param.sym, param.name);
    if (!param.defaultValue()) {
      this->lowering.storeLowerer().storeVar(param.sym, slot, arg, false);
      continue;
    }
    const Type *passed = Type::optional(param.sym->type);
    llvm::BasicBlock *givenBlock = this->lowering.functionFrame().newBlock("param.given");
    llvm::BasicBlock *defaultBlock = this->lowering.functionFrame().newBlock("param.default");
    llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("param.done");
    this->lowering.builder_.CreateCondBr(this->lowering.optionalLowerer().optionalHas(arg, passed),
                                         givenBlock, defaultBlock);
    this->lowering.functionFrame().switchTo(givenBlock);
    llvm::Value *given = param.sym->type->isOptional()
                             ? arg
                             : this->lowering.optionalLowerer().optionalValue(arg, passed);
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.functionFrame().switchTo(defaultBlock);
    llvm::Value *fallback = this->lowering.exprLowerer().emitExpr(param.defaultValue());
    llvm::BasicBlock *defaultEnd = this->lowering.builder_.GetInsertBlock();
    this->lowering.builder_.CreateBr(doneBlock);
    this->lowering.functionFrame().switchTo(doneBlock);
    llvm::PHINode *value =
        this->lowering.builder_.CreatePHI(this->lowering.moduleBuilder().lower(param.sym->type), 2);
    value->addIncoming(given, givenBlock);
    value->addIncoming(fallback, defaultEnd);
    this->lowering.storeLowerer().storeVar(param.sym, slot, value, false);
  }
}

void FunctionBuilder::finishFunction() {
  this->closeBody();
  this->lowering.function_ = nullptr;
}

void FunctionBuilder::closeBody() {
  if (this->lowering.functionFrame().blockIsOpen()) {
    llvm::Type *ret = this->lowering.function_->getReturnType();
    if (ret->isVoidTy()) {
      this->lowering.builder_.CreateRetVoid();
    } else if (this->lowering.function_->hasExternalLinkage()) {
      this->lowering.builder_.CreateRet(this->lowering.builder_.getInt32(0));
    } else if (this->lowering.returnType_ && (this->lowering.returnType_->isOptional() ||
                                              this->lowering.returnType_->isUnknown())) {
      this->lowering.builder_.CreateRet(
          this->lowering.optionalLowerer().undefinedValue(this->lowering.returnType_));
    } else {
      this->lowering.builder_.CreateUnreachable();
    }
  }
}

}
