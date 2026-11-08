#include "compiler/lower/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/StringExtras.h"
#include <string>

using llvm::cast;

namespace tz {

TzModulesLowerer::TzModulesLowerer(Lowering &owner)
    : lowering(owner) {}

void TzModulesLowerer::emitExternal(FunctionDecl *fn) {
  llvm::Function *function = this->lowering.functions_.lookup(fn->sym);
  this->lowering.functionBuilder().beginFunction(function, fn);
  const FuncSymbol *sym = fn->sym;
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  std::vector<llvm::Type *> params;
  std::vector<llvm::Value *> args;
  const Type *ret = sym->getKnownReturnType();
  llvm::Value *promise = nullptr;
  llvm::Value *text = nullptr;
  llvm::Type *cret = this->lowering.builder.getVoidTy();
  if (ret->isPromise()) {
    promise = this->lowering.promiseLowerer().newPromise(ret->promised());
    params.push_back(ptr);
    args.push_back(promise);
  } else if (ret->isString()) {
    text = this->lowering.functionFrame().createStackSlot(this->lowering.strTy, "text.out");
    this->lowering.builder.CreateStore(llvm::Constant::getNullValue(this->lowering.strTy), text);
    params.push_back(ptr);
    args.push_back(text);
  } else if (ret->isBool()) {
    cret = i32;
  } else if (ret->isResult()) {
    cret = this->lowering.moduleBuilder().lower(ret->okType());
  } else if (!ret->isVoid()) {
    cret = this->lowering.moduleBuilder().lower(ret);
  }
  for (size_t i = 0; i < sym->params.size(); ++i) {
    llvm::Value *value = function->getArg(unsigned(i));
    const Type *type = sym->params[i];
    if (type->isString()) {
      params.push_back(ptr);
      params.push_back(i64);
      args.push_back(this->lowering.stringLowerer().stringObj(value));
      args.push_back(this->lowering.stringLowerer().stringMeta(value));
    } else if (type->isBool()) {
      params.push_back(i32);
      args.push_back(this->lowering.builder.CreateZExt(value, i32));
    } else if (type->isFunction()) {
      params.push_back(ptr);
      params.push_back(ptr);
      args.push_back(this->lowering.builder.CreateExtractValue(value, 0));
      args.push_back(this->lowering.builder.CreateExtractValue(value, 1));
    } else {
      params.push_back(value->getType());
      args.push_back(value);
    }
  }
  params.push_back(ptr);
  if (this->lowering.nativeError) {
    args.push_back(this->lowering.fsLowerer().nativeErrorMaker());
  } else {
    args.push_back(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)));
  }
  std::string name = "tz_";
  for (char c : fn->name) {
    if (llvm::isUpper(c)) {
      name += '_';
    }
    name += llvm::toLower(c);
  }
  llvm::Value *result;
  if (llvm::StringRef(name).starts_with("tz_tls_") && !this->lowering.usesTls) {
    this->lowering.builder.CreateCall(
        this->lowering.processLowerer().processFunction(
            "tz_net_no_tls", this->lowering.builder.getVoidTy(), {ptr, ptr}),
        {promise ? promise : llvm::Constant::getNullValue(ptr), args.back()});
    result = cret->isVoidTy() ? nullptr : llvm::Constant::getNullValue(cret);
  } else {
    result = this->lowering.builder.CreateCall(
        this->lowering.processLowerer().processFunction(name, cret, params), args);
  }
  for (size_t i = 0; i < sym->params.size(); ++i) {
    this->lowering.refCounts().releaseValue(function->getArg(unsigned(i)), sym->params[i]);
  }
  if (promise) {
    this->lowering.builder.CreateRet(promise);
  } else if (text) {
    this->lowering.builder.CreateRet(this->lowering.builder.CreateLoad(this->lowering.strTy, text));
  } else if (ret->isBool()) {
    this->lowering.builder.CreateRet(
        this->lowering.builder.CreateICmpNE(result, this->lowering.builder.getInt32(0)));
  } else if (ret->isResult()) {
    this->lowering.builder.CreateRet(
        this->lowering.resultLowerer().catchFailure(result, ret, fn->loc()));
  } else if (ret->isVoid()) {
    this->lowering.builder.CreateRetVoid();
  } else {
    this->lowering.builder.CreateRet(result);
  }
  this->lowering.functionBuilder().finishFunction();
}

llvm::Value *TzModulesLowerer::emitResultAdapter(CastExpr *cast) {
  const Type *from = cast->operand->type;
  const Type *to = cast->type;
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(cast->operand);
  llvm::Constant *&info = this->resultAdapters[{from, to}];
  if (!info) {
    auto *code = llvm::Function::Create(this->lowering.closureLowerer().closureFnType(to),
                                        llvm::GlobalValue::InternalLinkage, "tz.adapt.result",
                                        this->lowering.module_.get());
    this->lowering.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilderBase::InsertPoint point = this->lowering.builder.saveIP();
    llvm::Function *outer = std::exchange(this->lowering.function, code);
    this->lowering.builder.SetInsertPoint(
        llvm::BasicBlock::Create(this->lowering.ctx, "entry", code));
    llvm::Value *inner = this->lowering.builder.CreateLoad(
        this->lowering.fnTy,
        this->lowering.builder.CreateStructGEP(this->lowering.envLowerer().boxType(from),
                                               code->getArg(0), 2),
        "wrapped");
    std::vector<llvm::Value *> args;
    llvm::ArrayRef<const Type *> mine = from->params();
    llvm::ArrayRef<const Type *> given = to->params();
    for (unsigned i = 1; i < code->arg_size(); ++i) {
      llvm::Value *arg = code->getArg(i);
      args.push_back(mine[i - 1] == given[i - 1] ? arg
                                                 : this->lowering.exprLowerer().emitConversion(
                                                       arg, given[i - 1], mine[i - 1]));
    }
    llvm::Value *result = this->lowering.closureLowerer().callFunctionValue(inner, from, args);
    this->lowering.builder.CreateRet(
        from->ret() == to->ret()
            ? result
            : this->lowering.exprLowerer().emitConversion(result, from->ret(), to->ret()));
    this->lowering.function = outer;
    this->lowering.builder.restoreIP(point);
    info = new llvm::GlobalVariable(
        *this->lowering.module_, this->lowering.fnInfoTy, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(this->lowering.fnInfoTy,
                                  {code, this->lowering.stringLowerer().stringLiteral(u"")}),
        "tz.fninfo.adapt");
  }
  bool member = this->lowering.memory().mayHoldMember(from);
  llvm::Value *box = this->lowering.envLowerer().newBox(from, member);
  this->lowering.builder.CreateStore(value, this->lowering.builder.CreateStructGEP(
                                                this->lowering.envLowerer().boxType(from), box, 2));
  if (member) {
    this->lowering.storeLowerer().groupStore(box, value, from);
  }

  return this->lowering.closureLowerer().makeFunctionValue(info, box);
}

void TzModulesLowerer::appendPrintHooks(llvm::ArrayRef<const ClassDecl *> chain,
                                        std::vector<llvm::Constant *> &entries) {
  std::vector<const FunctionDecl *> getters;
  for (const ClassDecl *c : chain) {
    for (const FunctionDecl *fn : c->methods) {
      if (fn->isStatic() || !fn->params.empty() || fn->name != "printed") {
        continue;
      }
      auto it = llvm::find_if(getters, [&](const FunctionDecl *g) { return g->name == fn->name; });
      if (it == getters.end()) {
        getters.push_back(fn);
      } else {
        *it = fn;
      }
    }
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  auto *getterTy = llvm::StructType::get(this->lowering.ctx, {this->lowering.typeDescTy, ptr, ptr});
  for (const FunctionDecl *fn : getters) {
    llvm::Function *code = fn->sym ? this->lowering.functions_.lookup(fn->sym) : nullptr;
    if (!code) {
      continue;
    }
    std::string name = (fn->cls()->name + "." + fn->name).str();
    auto *get = llvm::Function::Create(
        llvm::FunctionType::get(this->lowering.builder.getVoidTy(), {ptr, ptr}, false),
        llvm::GlobalValue::InternalLinkage, "tz.get." + name, this->lowering.module_.get());
    this->lowering.functionBuilder().setFunctionAttributes(get);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx, "entry", get));
    llvm::Value *value = b.CreateCall(code, {get->getArg(0)});
    b.CreateStore(value, get->getArg(1));
    b.CreateRetVoid();
    llvm::Constant *base = llvm::ConstantStruct::get(
        this->lowering.typeDescTy,
        {this->lowering.builder.getInt32(17), this->lowering.builder.getInt32(0),
         llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr))});
    auto *type = new llvm::GlobalVariable(
        *this->lowering.module_, getterTy, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(
            getterTy,
            {base, this->lowering.typeDescriptors().typeDescriptor(fn->sym->getKnownReturnType()),
             get}),
        "tz.getter." + name);
    entries.push_back(llvm::ConstantStruct::get(
        this->lowering.fieldDescTy, {this->lowering.stringLowerer().utf8Constant(fn->name),
                                     this->lowering.builder.getInt32(uint32_t(fn->name.size())),
                                     this->lowering.builder.getInt32(0xFFFFFFFEu), type}));
  }
}

}
