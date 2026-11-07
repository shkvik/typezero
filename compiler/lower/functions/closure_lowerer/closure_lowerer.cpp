#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

std::u16string ClosureLowerer::utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

std::string ClosureLowerer::qualifiedName(const FunctionDecl *fn) {
  std::string name = fn->name.empty() ? "lambda" : fn->name.str();
  for (const FunctionDecl *p = fn->parent; p; p = p->parent) {
    name = (p->name.empty() ? "lambda" : p->name.str()) + "." + name;
  }

  return "tz." + name;
}

FunctionDecl *ClosureLowerer::knownFunction(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name) {
    return nullptr;
  }
  if (name->func()) {
    return name->func()->decl;
  }
  VarSymbol *var = name->var();
  if (var) {
    return var->function() ? var->function() : var->isConst ? var->lambda() : nullptr;
  }

  return nullptr;
}

ClosureLowerer::ClosureLowerer(Lowering &owner)
    : lowering(owner) {}

const Type *ClosureLowerer::functionTypeOf(const FuncSymbol *fn) {
  return Type::function(fn->params, fn->minArgs, fn->hasRest, fn->ret, fn->hasThis);
}

const Type *ClosureLowerer::typeOf(const VarSymbol *var) {
  if (var->type) {
    return var->type;
  }
  const FunctionDecl *fn = var->function() ? var->function() : var->lambda();

  return ClosureLowerer::functionTypeOf(fn->sym);
}

llvm::FunctionType *ClosureLowerer::closureFnType(const Type *fnType) {
  std::vector<llvm::Type *> params{this->lowering.builder_.getPtrTy()};
  for (const Type *param : fnType->params()) {
    params.push_back(this->lowering.moduleBuilder().lower(param));
  }

  return llvm::FunctionType::get(this->lowering.moduleBuilder().lower(fnType->ret()), params,
                                 false);
}

llvm::Value *ClosureLowerer::makeFunctionValue(llvm::Value *info, llvm::Value *env) {
  auto *infoConst = dyn_cast<llvm::Constant>(info);
  if (infoConst) {
    auto *envConst = dyn_cast<llvm::Constant>(env);
    if (envConst) {
      return llvm::ConstantStruct::get(this->lowering.fnTy_, {infoConst, envConst});
    }
  }
  llvm::Value *value = llvm::UndefValue::get(this->lowering.fnTy_);
  value = this->lowering.builder_.CreateInsertValue(value, info, 0);

  return this->lowering.builder_.CreateInsertValue(value, env, 1);
}

llvm::Function *ClosureLowerer::closureCode(FunctionDecl *fn) {
  llvm::Function *code = this->closureCodes.lookup(fn);
  if (code) {
    return code;
  }
  code = llvm::Function::Create(this->closureFnType(ClosureLowerer::functionTypeOf(fn->sym)),
                                llvm::GlobalValue::InternalLinkage,
                                ClosureLowerer::qualifiedName(fn), this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(code);
  this->closureCodes[fn] = code;
  this->lowering.pendingClosures_.push_back(fn);

  return code;
}

llvm::Constant *ClosureLowerer::functionInfo(FunctionDecl *fn) {
  llvm::Constant *info = this->infos.lookup(fn);
  if (info) {
    return info;
  }
  llvm::Function *code;
  if (fn->takesThis()) {
    code = this->lowering.classDescriptors().methodCode(fn);
  } else if (fn->isClosure()) {
    code = this->closureCode(fn);
  } else {
    llvm::Function *target = this->lowering.functions_.lookup(fn->sym);
    code = llvm::Function::Create(this->closureFnType(ClosureLowerer::functionTypeOf(fn->sym)),
                                  llvm::GlobalValue::InternalLinkage, "tz." + fn->name + ".value",
                                  this->lowering.module_.get());
    this->lowering.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx_, "entry", code));
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < code->arg_size(); ++i) {
      args.push_back(code->getArg(i));
    }
    llvm::CallInst *result = b.CreateCall(target, args);
    if (result->getType()->isVoidTy()) {
      b.CreateRetVoid();
    } else {
      b.CreateRet(result);
    }
  }
  auto *global = new llvm::GlobalVariable(
      *this->lowering.module_, this->lowering.fnInfoTy_, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(
          this->lowering.fnInfoTy_,
          {code, this->lowering.stringLowerer().stringLiteral(ClosureLowerer::utf16(fn->name))}),
      "tz.fninfo." + fn->name);
  this->infos[fn] = global;

  return global;
}

llvm::Value *ClosureLowerer::emitFunctionExpr(FunctionExpr *fe) {
  FunctionDecl *fn = fe->fn;
  llvm::Constant *info = this->functionInfo(fn);
  const VarSymbol *var = fn->receiverEnv;
  if (var) {
    return this->makeFunctionValue(info, this->lowering.envLowerer().emitVarValue(var, false));
  }
  const Lowering::EnvLayout &layout = this->lowering.envLowerer().envLayout(fn);
  llvm::Value *env = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  if (!layout.fields.empty() || (this->lowering.comparesFunctions_ && !fn->nonEscaping)) {
    env = this->lowering.envLowerer().createEnv(
        layout, fn->nonEscaping, !this->lowering.memory().unstoredSites.contains(fe));
    this->lowering.groupLowerer().noteUnstored(env, fe);
  }

  return this->makeFunctionValue(info, env);
}

llvm::Value *ClosureLowerer::closureEnv(CallExpr *call) {
  FunctionDecl *fn = call->func()->decl;
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  if (this->lowering.currentFn_ && (this->lowering.currentFn_ == fn ||
                                    (fn->group && this->lowering.currentFn_->group == fn->group))) {
    return this->lowering.envParam_ ? this->lowering.envParam_ : null;
  }
  if (fn->group && fn->group->owner == this->lowering.currentFn_) {
    return this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(),
                                              this->lowering.groupEnvs_.lookup(fn->group));
  }
  const VarSymbol *var = cast<NameExpr>(call->callee)->var();

  return this->lowering.builder_.CreateExtractValue(
      this->lowering.envLowerer().emitVarValue(var, true), 1);
}

llvm::Value *ClosureLowerer::callFunctionValue(llvm::Value *fn, const Type *fnType,
                                               llvm::ArrayRef<llvm::Value *> args) {
  std::vector<llvm::Value *> all{this->lowering.builder_.CreateExtractValue(fn, 1)};
  all.insert(all.end(), args.begin(), args.end());
  for (size_t i = args.size(); i < fnType->params().size(); ++i) {
    all.push_back(this->lowering.optionalLowerer().undefinedValue(fnType->params()[i]));
  }
  llvm::Value *info = this->lowering.builder_.CreateExtractValue(fn, 0);
  llvm::Value *code = this->lowering.builder_.CreateLoad(
      this->lowering.builder_.getPtrTy(),
      this->lowering.builder_.CreateStructGEP(this->lowering.fnInfoTy_, info, 0), "code");
  llvm::CallInst *result =
      this->lowering.builder_.CreateCall(this->closureFnType(fnType), code, all);

  return result->getType()->isVoidTy() ? nullptr : result;
}

llvm::Value *ClosureLowerer::emitValueCall(CallExpr *call) {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member) {
    if (const Lowering::InterfaceMember *method = this->lowering.inheritLowerer().dispatchedMember(
            member->object->type, member->name, call->signature());
        (method && method->callable) || call->signature() >= 0) {
      return this->lowering.inheritLowerer().emitInterfaceCall(call);
    }
  }
  const Type *type = call->callee->type;
  Lowering::Operand callee = this->lowering.refCounts().emitOperand(
      call->callee,
      isa<NameExpr>(call->callee) && llvm::all_of(call->args, AssignLowerer::cannotAssign));
  std::vector<llvm::Value *> args = this->lowering.exprLowerer().emitExprs(call->args);
  llvm::Value *result = this->callFunctionValue(callee.value, type, args);
  this->lowering.refCounts().dropOperand(callee);

  return result;
}

llvm::Value *ClosureLowerer::emitFunctionAdapter(CastExpr *cast) {
  FunctionDecl *fn = ClosureLowerer::knownFunction(cast->operand);
  assert(fn && "Checker adapts only functions known by name");
  const Type *from = cast->operand->type;
  const Type *to = cast->type;
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(cast->operand);
  llvm::Constant *&info = this->adapters[{fn, to}];
  if (!info) {
    auto *code = llvm::Function::Create(this->closureFnType(to), llvm::GlobalValue::InternalLinkage,
                                        ClosureLowerer::qualifiedName(fn) + ".adapter",
                                        this->lowering.module_.get());
    this->lowering.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
    llvm::Function *outer = this->lowering.function_;
    this->lowering.function_ = code;
    this->lowering.builder_.SetInsertPoint(
        llvm::BasicBlock::Create(this->lowering.ctx_, "entry", code));
    std::vector<llvm::Value *> args;
    llvm::ArrayRef<const Type *> params = from->params();
    llvm::ArrayRef<const Type *> given = to->params();
    for (size_t i = 0; i < params.size(); ++i) {
      if (i >= given.size()) {
        args.push_back(this->lowering.optionalLowerer().undefinedValue(params[i]));
        continue;
      }
      llvm::Value *arg = code->getArg(unsigned(i + 1));
      if (params[i] == given[i]) {
        args.push_back(arg);
      } else if (params[i]->nonOptional()->isUnknown()) {
        args.push_back(this->lowering.exprLowerer().emitConversion(arg, given[i], params[i]));
      } else {
        args.push_back(this->lowering.optionalLowerer().makeOptional(
            arg, this->lowering.builder_.getTrue(), params[i]));
      }
    }
    for (size_t i = params.size(); i < given.size(); ++i) {
      this->lowering.refCounts().releaseValue(code->getArg(unsigned(i + 1)), given[i]);
    }
    llvm::CallInst *result;
    if (fn->isClosure()) {
      args.insert(args.begin(), code->getArg(0));
      result = this->lowering.builder_.CreateCall(this->closureCode(fn), args);
    } else {
      result = this->lowering.builder_.CreateCall(this->lowering.functions_.lookup(fn->sym), args);
    }
    if (to->ret()->isVoid()) {
      if (!from->ret()->isVoid()) {
        this->lowering.refCounts().releaseValue(result, from->ret());
      }
      this->lowering.builder_.CreateRetVoid();
    } else {
      this->lowering.builder_.CreateRet(result);
    }
    this->lowering.function_ = outer;
    this->lowering.builder_.restoreIP(point);
    info = new llvm::GlobalVariable(
        *this->lowering.module_, this->lowering.fnInfoTy_, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(
            this->lowering.fnInfoTy_,
            {code, this->lowering.stringLowerer().stringLiteral(ClosureLowerer::utf16(fn->name))}),
        "tz.fninfo." + fn->name);
  }

  return this->makeFunctionValue(info, this->lowering.builder_.CreateExtractValue(value, 1));
}

void ClosureLowerer::emitClosure(FunctionDecl *fn) {
  if (fn->isGenerator) {
    this->lowering.generatorLowerer().emitGenerator(fn, this->closureCode(fn));
    return;
  }
  if (fn->isAsync) {
    this->lowering.asyncLowerer().emitAsyncFunction(fn, this->closureCode(fn));
    return;
  }
  this->lowering.tailCallLowerer().findTailGroups(fn);
  Lowering::TailGroup *group = this->lowering.tailGroupOf_.lookup(fn);
  if (group) {
    this->lowering.tailCallLowerer().emitTailGroup(*group);
    return;
  }
  this->lowering.functionBuilder().beginFunction(this->closureCode(fn), fn);
  this->lowering.envParam_ = this->lowering.function_->getArg(0);
  this->lowering.envLowerer().bindCaptures(fn);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < this->lowering.function_->arg_size(); ++i) {
    args.push_back(this->lowering.function_->getArg(i));
  }
  this->lowering.functionBuilder().emitParamsAndBody(fn, args);
  this->lowering.functionBuilder().finishFunction();
}

void ClosureLowerer::emitPendingClosures() {
  while (!this->lowering.pendingClosures_.empty()) {
    FunctionDecl *fn = this->lowering.pendingClosures_.back();
    this->lowering.pendingClosures_.pop_back();
    this->emitClosure(fn);
  }
}

}
