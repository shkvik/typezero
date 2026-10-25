#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/library/builtin_modules/builtin_module_lowerer/builtin_module_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

TzModulesLowerer::TzModulesLowerer(CodeGen &codegen) : codegen_(codegen) {}

void TzModulesLowerer::emitExternal(FunctionDecl *fn) {
  llvm::Function *function = codegen_.functions_.lookup(fn->sym);
  codegen_.functionBuilder().beginFunction(function, fn);
  const FuncSymbol *sym = fn->sym;
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Type *i64 = codegen_.builder_.getInt64Ty();
  std::vector<llvm::Type *> params;
  std::vector<llvm::Value *> args;
  const Type *ret = sym->ret;
  llvm::Value *promise = nullptr, *text = nullptr;
  llvm::Type *cret = codegen_.builder_.getVoidTy();
  if (ret->isPromise()) {
    promise = codegen_.promiseLowerer().newPromise(ret->promised());
    params.push_back(ptr);
    args.push_back(promise);
  } else if (ret->isString()) {
    text = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "text.out");
    codegen_.builder_.CreateStore(llvm::Constant::getNullValue(codegen_.strTy_), text);
    params.push_back(ptr);
    args.push_back(text);
  } else if (ret->isBool()) {
    cret = i32;
  } else if (!ret->isVoid()) {
    cret = codegen_.moduleBuilder().lower(ret);
  }
  for (size_t i = 0; i < sym->params.size(); ++i) {
    llvm::Value *value = function->getArg(unsigned(i));
    const Type *type = sym->params[i];
    if (type->isString()) {
      params.push_back(ptr);
      params.push_back(i64);
      args.push_back(codegen_.stringLowerer().stringObj(value));
      args.push_back(codegen_.stringLowerer().stringMeta(value));
    } else if (type->isBool()) {
      params.push_back(i32);
      args.push_back(codegen_.builder_.CreateZExt(value, i32));
    } else if (LibraryCallLowerer::isAbortSignal(type)) {
      // A signal crosses as the runtime's side of it
      // (runtime/library/events/signal/signal.c).
      params.push_back(ptr);
      args.push_back(codegen_.libraryCallLowerer().nativeSignal(value, type));
    } else {
      params.push_back(value->getType());
      args.push_back(value);
    }
  }
  params.push_back(ptr);
  args.push_back(codegen_.builtinModuleLowerer().systemErrorMaker());
  llvm::StringRef name = fn->name;
  name.consume_front("__");
  llvm::Value *result;
  if (name.starts_with("tz_tls_") && !codegen_.usesTls_) {
    // A program that tz built without TLS (SourceFile::usesTls) has none to call: the call fails
    // as the runtime words it, rejecting the promise or throwing.
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_net_no_tls", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                        {promise ? promise : llvm::Constant::getNullValue(ptr), args.back()});
    result = cret->isVoidTy() ? nullptr : llvm::Constant::getNullValue(cret);
  } else {
    result = codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction(name, cret, params), args);
  }
  // The function owns its arguments; the runtime only borrowed them.
  for (size_t i = 0; i < sym->params.size(); ++i) {
    codegen_.refCounts().releaseValue(function->getArg(unsigned(i)), sym->params[i]);
  }
  if (promise) {
    codegen_.builder_.CreateRet(promise);
  } else if (text) {
    codegen_.builder_.CreateRet(codegen_.builder_.CreateLoad(codegen_.strTy_, text));
  } else if (ret->isBool()) {
    codegen_.builder_.CreateRet(codegen_.builder_.CreateICmpNE(result, codegen_.builder_.getInt32(0)));
  } else if (ret->isVoid()) {
    codegen_.builder_.CreateRetVoid();
  } else {
    codegen_.builder_.CreateRet(result);
  }
  codegen_.functionBuilder().finishFunction();
}

llvm::Value *TzModulesLowerer::emitResultAdapter(CastExpr *cast) {
  const Type *from = cast->operand->type, *to = cast->type;
  llvm::Value *value = codegen_.exprLowerer().emitExpr(cast->operand);
  llvm::Constant *&info = resultAdapters_[{from, to}];
  if (!info) {
    auto *code = llvm::Function::Create(codegen_.closureLowerer().closureFnType(to), llvm::GlobalValue::InternalLinkage,
                                        "tz.adapt.result", codegen_.module_.get());
    codegen_.functionBuilder().setFunctionAttributes(code);
    llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
    llvm::Function *outer = std::exchange(codegen_.function_, code);
    codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", code));
    // The environment is the box of the function it wraps, which the call
    // borrows; the arguments go to that function, which owns them.
    llvm::Value *inner = codegen_.builder_.CreateLoad(
        codegen_.fnTy_, codegen_.builder_.CreateStructGEP(codegen_.envLowerer().boxType(from), code->getArg(0), 2), "wrapped");
    std::vector<llvm::Value *> args;
    for (unsigned i = 1; i < code->arg_size(); ++i) {
      args.push_back(code->getArg(i));
    }
    llvm::Value *result = codegen_.closureLowerer().callFunctionValue(inner, from, args);
    // What it gives after it threw is nothing to convert.
    if (codegen_.typeThrows(from)) {
      llvm::BasicBlock *thrownBlock = codegen_.functionFrame().newBlock("adapt.thrown");
      llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("adapt.ok");
      llvm::Value *error = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.throwLowerer().thrownSlot(), "thrown");
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(error), thrownBlock, okBlock);
      codegen_.functionFrame().switchTo(thrownBlock);
      codegen_.builder_.CreateRet(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(to->ret())));
      codegen_.functionFrame().switchTo(okBlock);
    }
    codegen_.builder_.CreateRet(from->ret() == to->ret() ? result
                                                : codegen_.exprLowerer().emitConversion(result, from->ret(), to->ret()));
    codegen_.function_ = outer;
    codegen_.builder_.restoreIP(point);
    info = new llvm::GlobalVariable(
        *codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(codegen_.fnInfoTy_, {code, codegen_.stringLowerer().stringLiteral(u"")}), "tz.fninfo.adapt");
  }
  // The box is a member when the function may hold one, as the box of an
  // unknown is (CodeGen::boxValue): a box outside the groups would count
  // from outside it a reference that a group holding the adapter owns.
  bool member = codegen_.memoryAnalysis().mayHoldMember(from);
  llvm::Value *box = codegen_.envLowerer().newBox(from, member);
  codegen_.builder_.CreateStore(value, codegen_.builder_.CreateStructGEP(codegen_.envLowerer().boxType(from), box, 2));
  if (member) {
    codegen_.storeLowerer().groupStore(box, value, from);
  }

  return codegen_.closureLowerer().makeFunctionValue(info, box);
}

// A key of TZ_FIELD_GETTER for each getter: a TzGetterType with the type of
// the value and a function that calls the getter and writes what it gives.
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
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *getterTy = llvm::StructType::get(codegen_.ctx_, {codegen_.typeDescTy_, ptr, ptr});
  for (const FunctionDecl *fn : getters) {
    llvm::Function *code = fn->sym ? codegen_.functions_.lookup(fn->sym) : nullptr;
    if (!code) {
      continue;
    }
    std::string name = (fn->cls->name + "." + fn->name).str();
    auto *get =
        llvm::Function::Create(llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr, ptr}, false),
                               llvm::GlobalValue::InternalLinkage, "tz.get." + name, codegen_.module_.get());
    codegen_.functionBuilder().setFunctionAttributes(get);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", get));
    llvm::Value *value = b.CreateCall(code, {get->getArg(0)});
    b.CreateStore(value, get->getArg(1));
    b.CreateRetVoid();
    llvm::Constant *base = llvm::ConstantStruct::get(
        codegen_.typeDescTy_, {codegen_.builder_.getInt32(17), codegen_.builder_.getInt32(0),
                      llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr))});
    auto *type = new llvm::GlobalVariable(
        *codegen_.module_, getterTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(getterTy, {base, codegen_.typeDescriptors().typeDescriptor(fn->sym->ret), get}),
        "tz.getter." + name);
    entries.push_back(llvm::ConstantStruct::get(
        codegen_.fieldDescTy_, {codegen_.stringLowerer().utf8Constant(fn->name), codegen_.builder_.getInt32(uint32_t(fn->name.size())),
                       codegen_.builder_.getInt32(0xFFFFFFFEu), type})); // TZ_FIELD_GETTER
  }
}

} // namespace tz
