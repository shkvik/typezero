#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

TzModulesLowerer::TzModulesLowerer(Lowering &lowering) : lowering_(lowering) {}

void TzModulesLowerer::emitExternal(FunctionDecl *fn) {
  llvm::Function *function = lowering_.functions_.lookup(fn->sym);
  lowering_.functionBuilder().beginFunction(function, fn);
  const FuncSymbol *sym = fn->sym;
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *i64 = lowering_.builder_.getInt64Ty();
  std::vector<llvm::Type *> params;
  std::vector<llvm::Value *> args;
  const Type *ret = sym->ret;
  llvm::Value *promise = nullptr, *text = nullptr;
  llvm::Type *cret = lowering_.builder_.getVoidTy();
  if (ret->isPromise()) {
    promise = lowering_.promiseLowerer().newPromise(ret->promised());
    params.push_back(ptr);
    args.push_back(promise);
  } else if (ret->isString()) {
    text = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "text.out");
    lowering_.builder_.CreateStore(llvm::Constant::getNullValue(lowering_.strTy_), text);
    params.push_back(ptr);
    args.push_back(text);
  } else if (ret->isBool()) {
    cret = i32;
  } else if (ret->isResult()) {
    cret = lowering_.moduleBuilder().lower(ret->okType());
  } else if (!ret->isVoid()) {
    cret = lowering_.moduleBuilder().lower(ret);
  }
  for (size_t i = 0; i < sym->params.size(); ++i) {
    llvm::Value *value = function->getArg(unsigned(i));
    const Type *type = sym->params[i];
    if (type->isString()) {
      params.push_back(ptr);
      params.push_back(i64);
      args.push_back(lowering_.stringLowerer().stringObj(value));
      args.push_back(lowering_.stringLowerer().stringMeta(value));
    } else if (type->isBool()) {
      params.push_back(i32);
      args.push_back(lowering_.builder_.CreateZExt(value, i32));
    } else if (LibraryCallLowerer::isAbortSignal(type)) {
      params.push_back(ptr);
      args.push_back(lowering_.libraryCallLowerer().nativeSignal(value, type));
    } else {
      params.push_back(value->getType());
      args.push_back(value);
    }
  }
  params.push_back(ptr);
  args.push_back(lowering_.fsLowerer().systemErrorMaker());
  llvm::StringRef name = fn->name;
  name.consume_front("__");
  llvm::Value *result;
  if (name.starts_with("tz_tls_") && !lowering_.usesTls_) {
    lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_net_no_tls", lowering_.builder_.getVoidTy(),
                                                   {ptr, ptr}),
        {promise ? promise : llvm::Constant::getNullValue(ptr), args.back()});
    result = cret->isVoidTy() ? nullptr : llvm::Constant::getNullValue(cret);
  } else {
    result = lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction(name, cret, params), args);
  }
  for (size_t i = 0; i < sym->params.size(); ++i) {
    lowering_.refCounts().releaseValue(function->getArg(unsigned(i)), sym->params[i]);
  }
  if (promise) {
    lowering_.builder_.CreateRet(promise);
  } else if (text) {
    lowering_.builder_.CreateRet(lowering_.builder_.CreateLoad(lowering_.strTy_, text));
  } else if (ret->isBool()) {
    lowering_.builder_.CreateRet(
        lowering_.builder_.CreateICmpNE(result, lowering_.builder_.getInt32(0)));
  } else if (ret->isResult()) {
    lowering_.builder_.CreateRet(lowering_.resultLowerer().catchFailure(result, ret, fn->loc()));
  } else if (ret->isVoid()) {
    lowering_.builder_.CreateRetVoid();
  } else {
    lowering_.builder_.CreateRet(result);
  }
  lowering_.functionBuilder().finishFunction();
}

llvm::Value *TzModulesLowerer::emitResultAdapter(CastExpr *cast) {
  const Type *from = cast->operand->type, *to = cast->type;
  llvm::Value *value = lowering_.exprLowerer().emitExpr(cast->operand);
  llvm::Constant *&info = resultAdapters_[{from, to}];
  if (!info) {
    auto *code = llvm::Function::Create(lowering_.closureLowerer().closureFnType(to),
                                        llvm::GlobalValue::InternalLinkage, "tz.adapt.result",
                                        lowering_.module_.get());
    lowering_.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
    llvm::Function *outer = std::exchange(lowering_.function_, code);
    lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", code));
    llvm::Value *inner =
        lowering_.builder_.CreateLoad(lowering_.fnTy_,
                                      lowering_.builder_.CreateStructGEP(
                                          lowering_.envLowerer().boxType(from), code->getArg(0), 2),
                                      "wrapped");
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < code->arg_size(); ++i) {
      args.push_back(code->getArg(i));
    }
    llvm::Value *result = lowering_.closureLowerer().callFunctionValue(inner, from, args);
    if (false) {
      llvm::BasicBlock *thrownBlock = lowering_.functionFrame().newBlock("adapt.thrown");
      llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("adapt.ok");
      llvm::Value *error = lowering_.builder_.CreateLoad(
          lowering_.builder_.getPtrTy(), lowering_.throwLowerer().thrownSlot(), "thrown");
      lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(error), thrownBlock,
                                      okBlock);
      lowering_.functionFrame().switchTo(thrownBlock);
      lowering_.builder_.CreateRet(
          llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(to->ret())));
      lowering_.functionFrame().switchTo(okBlock);
    }
    lowering_.builder_.CreateRet(from->ret() == to->ret() ? result
                                                          : lowering_.exprLowerer().emitConversion(
                                                                result, from->ret(), to->ret()));
    lowering_.function_ = outer;
    lowering_.builder_.restoreIP(point);
    info = new llvm::GlobalVariable(
        *lowering_.module_, lowering_.fnInfoTy_, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(lowering_.fnInfoTy_,
                                  {code, lowering_.stringLowerer().stringLiteral(u"")}),
        "tz.fninfo.adapt");
  }
  bool member = lowering_.memory().mayHoldMember(from);
  llvm::Value *box = lowering_.envLowerer().newBox(from, member);
  lowering_.builder_.CreateStore(
      value, lowering_.builder_.CreateStructGEP(lowering_.envLowerer().boxType(from), box, 2));
  if (member) {
    lowering_.storeLowerer().groupStore(box, value, from);
  }

  return lowering_.closureLowerer().makeFunctionValue(info, box);
}

void TzModulesLowerer::appendGetterKeys(llvm::ArrayRef<const ClassDecl *> chain,
                                        std::vector<llvm::Constant *> &entries) {
  std::vector<const FunctionDecl *> getters;
  for (const ClassDecl *c : chain) {
    for (const FunctionDecl *fn : c->methods) {
      if (fn->accessor != FunctionDecl::Accessor::Getter || fn->isStatic ||
          fn->access != Access::Public || fn->name.starts_with("#") || fn->hiddenGetter) {
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
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  auto *getterTy = llvm::StructType::get(lowering_.ctx_, {lowering_.typeDescTy_, ptr, ptr});
  for (const FunctionDecl *fn : getters) {
    llvm::Function *code = fn->sym ? lowering_.functions_.lookup(fn->sym) : nullptr;
    if (!code) {
      continue;
    }
    std::string name = (fn->cls->name + "." + fn->name).str();
    auto *get = llvm::Function::Create(
        llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr, ptr}, false),
        llvm::GlobalValue::InternalLinkage, "tz.get." + name, lowering_.module_.get());
    lowering_.functionBuilder().setFunctionAttributes(get);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", get));
    llvm::Value *value = b.CreateCall(code, {get->getArg(0)});
    b.CreateStore(value, get->getArg(1));
    b.CreateRetVoid();
    llvm::Constant *base = llvm::ConstantStruct::get(
        lowering_.typeDescTy_,
        {lowering_.builder_.getInt32(17), lowering_.builder_.getInt32(0),
         llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr))});
    auto *type = new llvm::GlobalVariable(
        *lowering_.module_, getterTy, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(
            getterTy, {base, lowering_.typeDescriptors().typeDescriptor(fn->sym->ret), get}),
        "tz.getter." + name);
    entries.push_back(llvm::ConstantStruct::get(
        lowering_.fieldDescTy_, {lowering_.stringLowerer().utf8Constant(fn->name),
                                 lowering_.builder_.getInt32(uint32_t(fn->name.size())),
                                 lowering_.builder_.getInt32(0xFFFFFFFEu), type}));
  }
}

}
