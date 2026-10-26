#include "compiler/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

std::string className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->constructors.empty()) {
      return c->constructors;
    }
  }

  return {};
}

}

ClassInfoLowerer::ClassInfoLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::StructType *ClassInfoLowerer::classInfoType() {
  if (!classInfoTy_) {
    llvm::Type *ptr = lowering_.builder_.getPtrTy();
    classInfoTy_ = llvm::StructType::create(
        lowering_.ctx_, {ptr, ptr, ptr, lowering_.builder_.getInt32Ty()}, "tz.classinfo");
  }

  return classInfoTy_;
}

llvm::Constant *ClassInfoLowerer::classInfoGlobal(const ClassDecl *cls) {
  llvm::GlobalVariable *&global = lowering_.classInfos_[cls];
  if (!global) {
    global = new llvm::GlobalVariable(
        *lowering_.module_, classInfoType(), true, llvm::GlobalValue::PrivateLinkage,
        llvm::Constant::getNullValue(classInfoType()),
        "tz.classinfo." + lowering_.symbolPrefixes_.lookup(cls) + className(cls));
  }

  return global;
}

void ClassInfoLowerer::emitConstructThunk(const ClassDecl *cls, llvm::Function *fn) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  lowering_.functionBuilder().beginFunction(fn, nullptr);
  lowering_.loc_ = cls->loc();
  lowering_.functionFrame().pushScope();
  llvm::Value *args = fn->getArg(0), *count = fn->getArg(1);
  std::string what = "new " + className(cls);
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  auto fail = [&] {
    lowering_.functionFrame().emitCleanups(0);
    lowering_.builder_.CreateRet(null);
  };
  std::vector<FunctionDecl *> ctors = constructorsOf(cls);
  bool precise = ctors.size() <= 1;
  llvm::BasicBlock *none = precise ? nullptr : lowering_.functionFrame().newBlock("reflect.none");
  llvm::BasicBlock *several =
      precise ? nullptr : lowering_.functionFrame().newBlock("reflect.several");
  std::vector<const FuncSymbol *> variants;
  for (FunctionDecl *ctor : ctors) {
    variants.push_back(ctor->sym);
  }
  if (variants.empty()) {
    variants.push_back(nullptr);
  }
  std::vector<llvm::BasicBlock *> starts;
  if (!precise) {
    std::vector<const FunctionDecl *> choices(ctors.begin(), ctors.end());
    llvm::Value *chosen =
        lowering_.reflectArgsLowerer().emitChooseVariant(choices, nullptr, args, count);
    llvm::SwitchInst *which =
        lowering_.builder_.CreateSwitch(chosen, none, unsigned(ctors.size() + 1));
    which->addCase(lowering_.builder_.getInt32(uint32_t(-2)), several);
    for (size_t v = 0; v < ctors.size(); ++v) {
      starts.push_back(lowering_.functionFrame().newBlock("reflect.variant"));
      which->addCase(lowering_.builder_.getInt32(uint32_t(v)), starts.back());
    }
  }
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v];
    if (!precise) {
      lowering_.functionFrame().switchTo(starts[v]);
    }
    llvm::BasicBlock *next = none;
    size_t min = sym ? sym->minArgs : 0;
    bool rest = sym && sym->hasRest;
    std::vector<const Type *> params = sym ? sym->params : std::vector<const Type *>();
    const Type *restType = rest ? params.back() : nullptr;
    if (rest) {
      params.pop_back();
    }
    llvm::Value *fits =
        lowering_.builder_.CreateICmpUGE(count, lowering_.builder_.getInt32(uint32_t(min)));
    if (!rest) {
      fits = lowering_.builder_.CreateAnd(
          fits, lowering_.builder_.CreateICmpULE(
                    count, lowering_.builder_.getInt32(uint32_t(params.size()))));
    }
    llvm::BasicBlock *arityOk = lowering_.functionFrame().newBlock("reflect.arity");
    llvm::BasicBlock *arityWrong = lowering_.functionFrame().newBlock("reflect.arity.wrong");
    lowering_.builder_.CreateCondBr(fits, arityOk, arityWrong);
    lowering_.functionFrame().switchTo(arityWrong);
    if (precise) {
      lowering_.builder_.CreateCall(
          lowering_.unknownLowerer().unknownFunction(
              "tz_reflect_arity", lowering_.builder_.getVoidTy(), {ptr, i32, i32, i32, i32}),
          {lowering_.stringLowerer().utf8Constant(what),
           lowering_.builder_.getInt32(uint32_t(what.size())),
           lowering_.builder_.getInt32(uint32_t(min)),
           lowering_.builder_.getInt32(rest ? -1 : int32_t(params.size())), count});
      fail();
    } else {
      lowering_.builder_.CreateBr(next);
    }
    lowering_.functionFrame().switchTo(arityOk);
    std::vector<llvm::Value *> values;
    lowering_.reflectArgsLowerer().emitReflectArgs(
        args, count, min, params, restType, values,
        [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
          if (!precise) {
            lowering_.builder_.CreateBr(next);
            return;
          }
          lowering_.reflectArgsLowerer().emitReflectMismatch(what, i, expected, got);
          fail();
        });
    llvm::Value *object = lowering_.classLowerer().allocInstance(cls);
    Lowering::Held held = lowering_.throwLowerer().hold(
        object, cls->type, lowering_.classLowerer().constructionThrows(cls, sym));
    lowering_.classLowerer().emitConstruct(object, cls, sym, values);
    llvm::Value *made = lowering_.throwLowerer().take(held);
    lowering_.functionFrame().emitCleanups(0);
    lowering_.builder_.CreateRet(made);
  }
  if (none) {
    lowering_.functionFrame().switchTo(none);
    lowering_.reflectArgsLowerer().emitReflectFail(
        "no_variant", what + ": no constructor of " + className(cls) + " takes these arguments");
    fail();
    lowering_.functionFrame().switchTo(several);
    lowering_.reflectArgsLowerer().emitReflectFail(
        "several_variants",
        what + ": the arguments fit several constructors of " + className(cls) + " equally well");
    fail();
  }
  lowering_.functionFrame().startDeadBlock();
  lowering_.functionFrame().popScope();
  lowering_.functionBuilder().finishFunction();
}

llvm::Constant *ClassInfoLowerer::reflectAdapterInfo(const FunctionDecl *fn, const Type *&type) {
  const FuncSymbol *sym = fn->sym;
  std::vector<const Type *> params{Type::unknown()};
  params.insert(params.end(), sym->params.begin(), sym->params.end());
  type = Type::function(params, sym->minArgs + 1, sym->hasRest, sym->ret, true);
  if (llvm::Constant *found = reflectAdapters_.lookup(fn)) {
    return found;
  }
  llvm::Function *method = lowering_.classDescriptors().methodCode(fn);
  std::string name = method->getName().str();
  auto *adapter = llvm::Function::Create(lowering_.closureLowerer().closureFnType(type),
                                         llvm::GlobalValue::InternalLinkage, name + ".reflect",
                                         lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      lowering_.fnInfoTy_, {adapter, lowering_.stringLowerer().stringLiteral(
                                         std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global =
      new llvm::GlobalVariable(*lowering_.module_, lowering_.fnInfoTy_, true,
                               llvm::GlobalValue::PrivateLinkage, info, name + ".reflect.info");
  reflectAdapters_[fn] = global;

  llvm::IRBuilderBase::InsertPointGuard guard(lowering_.builder_);
  llvm::Function *outer = std::exchange(lowering_.function_, adapter);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", adapter));
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = sym->ret;
  auto finish = [&](llvm::Value *result) {
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {self});
    if (ret->isVoid()) {
      lowering_.builder_.CreateRetVoid();
    } else {
      lowering_.builder_.CreateRet(result);
    }
  };
  const ClassDecl *cls = fn->cls;
  llvm::BasicBlock *failBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.wrong", adapter);
  llvm::BasicBlock *testBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.test", adapter);
  llvm::BasicBlock *callBlock = llvm::BasicBlock::Create(lowering_.ctx_, "this.ok", adapter);
  llvm::Value *layout = lowering_.builder_.CreateCall(
      lowering_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {self},
      "layout");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(layout), testBlock, failBlock);
  lowering_.builder_.SetInsertPoint(testBlock);
  llvm::Value *id = lowering_.builder_.CreateLoad(
      lowering_.builder_.getInt32Ty(),
      lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, layout, 6), "class");
  unsigned last = std::max(cls->id, cls->lastId);
  llvm::Value *matches = lowering_.builder_.CreateICmpULE(
      lowering_.builder_.CreateSub(id, lowering_.builder_.getInt32(cls->id)),
      lowering_.builder_.getInt32(last - cls->id));
  lowering_.builder_.CreateCondBr(matches, callBlock, failBlock);
  lowering_.builder_.SetInsertPoint(failBlock);
  lowering_.reflectArgsLowerer().emitReflectFail(
      "bad_this", className(cls) + "." + fn->name.str() +
                      " was called with a 'this' that is not an instance of " + className(cls));
  for (size_t i = 1; i < params.size(); ++i) {
    lowering_.refCounts().releaseValue(adapter->getArg(unsigned(i + 1)), params[i]);
  }
  finish(ret->isVoid() ? nullptr
                       : llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(ret)));
  lowering_.builder_.SetInsertPoint(callBlock);
  std::vector<llvm::Value *> args{self};
  for (unsigned i = 2; i < adapter->arg_size(); ++i) {
    args.push_back(adapter->getArg(i));
  }
  llvm::CallInst *result = lowering_.builder_.CreateCall(method, args);
  finish(ret->isVoid() ? nullptr : result);
  lowering_.function_ = outer;

  return global;
}

std::vector<const FunctionDecl *> ClassInfoLowerer::reflectVariants(const ClassDecl *cls,
                                                                    llvm::StringRef name) {
  auto overrides = [](const FunctionDecl *fn, const FunctionDecl *base) {
    for (const FunctionDecl *p = fn; p; p = p->overrides) {
      if (p == base) {
        return true;
      }
    }

    return false;
  };
  std::vector<const FunctionDecl *> found;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<const FunctionDecl *> own(c->methods.begin(), c->methods.end());
    for (const ClassField &f : c->fields) {
      if (f.method) {
        own.push_back(f.method);
      }
    }
    for (const FunctionDecl *fn : own) {
      if (fn->name == name && !fn->isStatic && !fn->isAbstract && fn->typeParams.empty() &&
          fn->sym && fn->sym->ret &&
          llvm::none_of(found, [&](const FunctionDecl *v) { return overrides(v, fn); })) {
        found.push_back(fn);
      }
    }
  }

  return found;
}

}
