#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

FunctionBuilder::FunctionBuilder(Lowering &lowering) : lowering_(lowering) {}

void FunctionBuilder::setFunctionAttributes(llvm::Function *fn) {
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  fn->setUWTableKind(llvm::UWTableKind::Default);
}

void FunctionBuilder::declareFunction(FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;
  std::vector<llvm::Type *> params;
  for (const Type *param : sym->params) {
    params.push_back(lowering_.moduleBuilder().lower(param));
  }
  auto *type = llvm::FunctionType::get(lowering_.moduleBuilder().lower(sym->ret), params, false);
  std::string name = "tz." + lowering_.symbolPrefixes_.lookup(fn) + fn->name.str();
  if (fn->cls) {
    name = "tz." + lowering_.symbolPrefixes_.lookup(fn->cls) + fn->cls->name.str() + "." +
           fn->name.str() + (fn->variant ? "." + std::to_string(fn->variant) : "");
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
                                          lowering_.module_.get());
  setFunctionAttributes(function);
  lowering_.functions_[sym] = function;
}

void FunctionBuilder::beginFunction(llvm::Function *function, FunctionDecl *fn) {
  lowering_.function_ = function;
  lowering_.currentFn_ = fn;
  lowering_.returnType_ = fn ? fn->sym->ret : nullptr;
  lowering_.envParam_ = nullptr;
  lowering_.addresses_.clear();
  lowering_.boxSlots_.clear();
  lowering_.envBoxes_.clear();
  lowering_.groupEnvs_.clear();
  lowering_.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(lowering_.ctx_, "entry", lowering_.function_));
}

void FunctionBuilder::emitFunction(FunctionDecl *fn) {
  if (fn->external) {
    lowering_.tzModulesLowerer().emitExternal(fn);
    return;
  }
  if (fn->impl) {
    lowering_.decoratorLowerer().emitDecoratedMethod(fn);
    return;
  }
  if (fn->isGenerator) {
    lowering_.generatorLowerer().emitGenerator(fn, lowering_.functions_.lookup(fn->sym));
    return;
  }
  if (fn->isAsync) {
    lowering_.asyncLowerer().emitAsyncFunction(fn, lowering_.functions_.lookup(fn->sym));
    return;
  }
  if (Lowering::TailGroup *group = lowering_.tailGroupOf_.lookup(fn)) {
    lowering_.tailCallLowerer().emitTailGroup(*group);
    return;
  }
  beginFunction(lowering_.functions_.lookup(fn->sym), fn);
  std::vector<llvm::Value *> args;
  for (llvm::Argument &arg : lowering_.function_->args()) {
    args.push_back(&arg);
  }
  emitParamsAndBody(fn, args);
  finishFunction();
}

void FunctionBuilder::emitParamsAndBody(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  lowering_.functionFrame().pushScope();
  emitParams(fn, args);
  if (fn && fn->isClosure() && !fn->group) {
    for (const VarSymbol *var : fn->captures) {
      if (lowering_.memory().uncounted.count({fn, var})) {
        const Type *type = ClosureLowerer::typeOf(var);
        llvm::Value *owner =
            lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type),
                                          lowering_.functionFrame().addressOf(var), var->name);
        lowering_.refCounts().retainValue(owner, type);
        lowering_.builder_.CreateStore(
            owner, lowering_.functionFrame().createManagedSlot(type, var->name + ".held"));
      }
    }
  }
  lowering_.stmtLowerer().emitStmt(fn->body);
  lowering_.functionFrame().popScope();
}

void FunctionBuilder::emitParams(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args) {
  if (fn->thisVar) {
    llvm::Value *slot =
        lowering_.functionFrame().createStackSlot(lowering_.builder_.getPtrTy(), "this");
    lowering_.builder_.CreateStore(lowering_.envParam_, slot);
    lowering_.addresses_[fn->thisVar] = slot;
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    llvm::Value *arg = args[i];
    if (isa<llvm::Argument>(arg)) {
      arg->setName(param.name);
    }
    llvm::Value *slot = lowering_.functionFrame().createLocal(param.sym, param.name);
    if (!param.defaultValue) {
      lowering_.storeLowerer().storeVar(param.sym, slot, arg, false);
      continue;
    }
    const Type *passed = Type::optional(param.sym->type);
    llvm::BasicBlock *givenBlock = lowering_.functionFrame().newBlock("param.given");
    llvm::BasicBlock *defaultBlock = lowering_.functionFrame().newBlock("param.default");
    llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("param.done");
    lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(arg, passed),
                                    givenBlock, defaultBlock);
    lowering_.functionFrame().switchTo(givenBlock);
    llvm::Value *given = param.sym->type->isOptional()
                             ? arg
                             : lowering_.optionalLowerer().optionalValue(arg, passed);
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(defaultBlock);
    llvm::Value *fallback = lowering_.exprLowerer().emitExpr(param.defaultValue);
    llvm::BasicBlock *defaultEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(doneBlock);
    lowering_.functionFrame().switchTo(doneBlock);
    llvm::PHINode *value =
        lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(param.sym->type), 2);
    value->addIncoming(given, givenBlock);
    value->addIncoming(fallback, defaultEnd);
    lowering_.storeLowerer().storeVar(param.sym, slot, value, false);
  }
}

void FunctionBuilder::finishFunction() {
  closeBody();
  lowering_.function_ = nullptr;
}

void FunctionBuilder::closeBody() {
  if (lowering_.functionFrame().blockIsOpen()) {
    llvm::Type *ret = lowering_.function_->getReturnType();
    if (ret->isVoidTy()) {
      lowering_.builder_.CreateRetVoid();
    } else if (lowering_.function_->hasExternalLinkage()) {
      lowering_.builder_.CreateRet(lowering_.builder_.getInt32(0));
    } else if (lowering_.returnType_ &&
               (lowering_.returnType_->isOptional() || lowering_.returnType_->isUnknown())) {
      lowering_.builder_.CreateRet(
          lowering_.optionalLowerer().undefinedValue(lowering_.returnType_));
    } else {
      lowering_.builder_.CreateUnreachable();
    }
  }
}

}
