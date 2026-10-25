#include "compiler/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// What a class is called in messages: its name, or what it extends.
std::string className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

// The constructors `new` of a class chooses from: its own, or those of the
// nearest class up its chain that has any.
std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->constructors.empty()) {
      return c->constructors;
    }
  }

  return {};
}

} // namespace

ClassInfoLowerer::ClassInfoLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::StructType *ClassInfoLowerer::classInfoType() {
  // TzClassInfo: { ptr construct, ptr from_json, ptr methods, i32 count }.
  if (!classInfoTy_) {
    llvm::Type *ptr = codegen_.builder_.getPtrTy();
    classInfoTy_ =
        llvm::StructType::create(codegen_.ctx_, {ptr, ptr, ptr, codegen_.builder_.getInt32Ty()}, "tz.classinfo");
  }

  return classInfoTy_;
}

llvm::Constant *ClassInfoLowerer::classInfoGlobal(const ClassDecl *cls) {
  llvm::GlobalVariable *&global = codegen_.classInfos_[cls];
  if (!global) {
    // Filled at the end (finishReflection), once the program is emitted.
    global = new llvm::GlobalVariable(
        *codegen_.module_, classInfoType(), /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::Constant::getNullValue(classInfoType()),
        "tz.classinfo." + codegen_.symbolPrefixes_.lookup(cls) + className(cls));
  }

  return global;
}

// (ptr args, i32 count) -> ptr: a new object of the class from arguments as
// unknowns, through its constructor (a mismatch names the argument); of
// several, through the variant the arguments fit best, as for a method
// (docs/overloads.md, section 6). NULL after throwing.
void ClassInfoLowerer::emitConstructThunk(const ClassDecl *cls, llvm::Function *fn) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  codegen_.functionBuilder().beginFunction(fn, nullptr);
  codegen_.loc_ = cls->loc();
  codegen_.functionFrame().pushScope();
  llvm::Value *args = fn->getArg(0), *count = fn->getArg(1);
  std::string what = "new " + className(cls);
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  auto fail = [&] {
    codegen_.functionFrame().emitCleanups(0);
    codegen_.builder_.CreateRet(null);
  };
  std::vector<FunctionDecl *> ctors = constructorsOf(cls);
  bool precise = ctors.size() <= 1;
  llvm::BasicBlock *none = precise ? nullptr : codegen_.functionFrame().newBlock("reflect.none");
  llvm::BasicBlock *several = precise ? nullptr : codegen_.functionFrame().newBlock("reflect.several");
  // A class without a constructor takes no arguments.
  std::vector<const FuncSymbol *> variants;
  for (FunctionDecl *ctor : ctors) {
    variants.push_back(ctor->sym);
  }
  if (variants.empty()) {
    variants.push_back(nullptr);
  }
  // Of several, the one the arguments fit best.
  std::vector<llvm::BasicBlock *> starts;
  if (!precise) {
    std::vector<const FunctionDecl *> choices(ctors.begin(), ctors.end());
    llvm::Value *chosen = codegen_.reflectArgsLowerer().emitChooseVariant(choices, nullptr, args, count);
    llvm::SwitchInst *which = codegen_.builder_.CreateSwitch(chosen, none, unsigned(ctors.size() + 1));
    which->addCase(codegen_.builder_.getInt32(uint32_t(-2)), several);
    for (size_t v = 0; v < ctors.size(); ++v) {
      starts.push_back(codegen_.functionFrame().newBlock("reflect.variant"));
      which->addCase(codegen_.builder_.getInt32(uint32_t(v)), starts.back());
    }
  }
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v];
    if (!precise) {
      codegen_.functionFrame().switchTo(starts[v]);
    }
    llvm::BasicBlock *next = none; // which the choice rules out
    size_t min = sym ? sym->minArgs : 0;
    bool rest = sym && sym->hasRest;
    std::vector<const Type *> params = sym ? sym->params : std::vector<const Type *>();
    const Type *restType = rest ? params.back() : nullptr;
    if (rest) {
      params.pop_back();
    }
    // How many it takes.
    llvm::Value *fits = codegen_.builder_.CreateICmpUGE(count, codegen_.builder_.getInt32(uint32_t(min)));
    if (!rest) {
      fits = codegen_.builder_.CreateAnd(
          fits, codegen_.builder_.CreateICmpULE(count, codegen_.builder_.getInt32(uint32_t(params.size()))));
    }
    llvm::BasicBlock *arityOk = codegen_.functionFrame().newBlock("reflect.arity");
    llvm::BasicBlock *arityWrong = codegen_.functionFrame().newBlock("reflect.arity.wrong");
    codegen_.builder_.CreateCondBr(fits, arityOk, arityWrong);
    codegen_.functionFrame().switchTo(arityWrong);
    if (precise) {
      codegen_.builder_.CreateCall(
          codegen_.unknownLowerer().unknownFunction("tz_reflect_arity", codegen_.builder_.getVoidTy(), {ptr, i32, i32, i32, i32}),
          {codegen_.stringLowerer().utf8Constant(what), codegen_.builder_.getInt32(uint32_t(what.size())),
           codegen_.builder_.getInt32(uint32_t(min)), codegen_.builder_.getInt32(rest ? -1 : int32_t(params.size())),
           count});
      fail();
    } else {
      codegen_.builder_.CreateBr(next);
    }
    codegen_.functionFrame().switchTo(arityOk);
    std::vector<llvm::Value *> values;
    codegen_.reflectArgsLowerer().emitReflectArgs(args, count, min, params, restType, values,
                    [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
                      if (!precise) {
                        codegen_.builder_.CreateBr(next);
                        return;
                      }
                      codegen_.reflectArgsLowerer().emitReflectMismatch(what, i, expected, got);
                      fail();
                    });
    llvm::Value *object = codegen_.classLowerer().allocInstance(cls);
    CodeGen::Held held = codegen_.throwLowerer().hold(object, cls->type, codegen_.classLowerer().constructionThrows(cls, sym));
    codegen_.classLowerer().emitConstruct(object, cls, sym, values);
    llvm::Value *made = codegen_.throwLowerer().take(held);
    codegen_.functionFrame().emitCleanups(0);
    codegen_.builder_.CreateRet(made);
  }
  auto throwText = [&](const std::string &message) {
    llvm::Value *text = codegen_.stringLowerer().stringLiteral(std::u16string(message.begin(), message.end()));
    codegen_.builder_.CreateCall(
        codegen_.unknownLowerer().unknownFunction("tz_throw_str", codegen_.builder_.getVoidTy(), {i32, ptr, codegen_.builder_.getInt64Ty()}),
        {codegen_.builder_.getInt32(1), codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text)});
    fail();
  };
  if (none) {
    codegen_.functionFrame().switchTo(none);
    throwText(what + ": no constructor of " + className(cls) + " takes these arguments");
    codegen_.functionFrame().switchTo(several);
    throwText(what + ": the arguments fit several constructors of " + className(cls) +
              " equally well");
  }
  codegen_.functionFrame().startDeadBlock();
  codegen_.functionFrame().popScope();
  codegen_.functionBuilder().finishFunction();
}

// The method as a function value that takes its `this`: `(env, this:
// unknown, params...) -> R`, which checks that the `this` is an object of the
// method's class (TypeError otherwise) and calls the method with it. The
// callee owns its `this`, as a function with a `this` parameter does.
llvm::Constant *ClassInfoLowerer::reflectAdapterInfo(const FunctionDecl *fn, const Type *&type) {
  const FuncSymbol *sym = fn->sym;
  std::vector<const Type *> params{Type::unknown()};
  params.insert(params.end(), sym->params.begin(), sym->params.end());
  type = Type::function(params, sym->minArgs + 1, sym->hasRest, sym->ret, /*hasThis=*/true);
  if (llvm::Constant *found = reflectAdapters_.lookup(fn)) {
    return found;
  }
  llvm::Function *method = codegen_.classDescriptors().methodCode(fn);
  std::string name = method->getName().str();
  auto *adapter = llvm::Function::Create(codegen_.closureLowerer().closureFnType(type), llvm::GlobalValue::InternalLinkage,
                                         name + ".reflect", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(adapter);
  llvm::Constant *info = llvm::ConstantStruct::get(
      codegen_.fnInfoTy_, {adapter, codegen_.stringLowerer().stringLiteral(std::u16string(fn->name.begin(), fn->name.end()))});
  auto *global =
      new llvm::GlobalVariable(*codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, info, name + ".reflect.info");
  reflectAdapters_[fn] = global;

  llvm::IRBuilderBase::InsertPointGuard guard(codegen_.builder_);
  llvm::Function *outer = std::exchange(codegen_.function_, adapter);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", adapter));
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  llvm::Value *self = adapter->getArg(1);
  const Type *ret = sym->ret;
  auto finish = [&](llvm::Value *result) {
    codegen_.builder_.CreateCall(codegen_.instanceReleaseFn_, {self});
    if (ret->isVoid()) {
      codegen_.builder_.CreateRetVoid();
    } else {
      codegen_.builder_.CreateRet(result);
    }
  };
  const ClassDecl *cls = fn->cls;
  llvm::BasicBlock *failBlock = llvm::BasicBlock::Create(codegen_.ctx_, "this.wrong", adapter);
  llvm::BasicBlock *testBlock = llvm::BasicBlock::Create(codegen_.ctx_, "this.test", adapter);
  llvm::BasicBlock *callBlock = llvm::BasicBlock::Create(codegen_.ctx_, "this.ok", adapter);
  llvm::Value *layout =
      codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {self}, "layout");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(layout), testBlock, failBlock);
  codegen_.builder_.SetInsertPoint(testBlock);
  llvm::Value *id = codegen_.builder_.CreateLoad(
      codegen_.builder_.getInt32Ty(), codegen_.builder_.CreateStructGEP(codegen_.layoutDescTy_, layout, 6), "class");
  unsigned last = std::max(cls->id, cls->lastId);
  llvm::Value *matches = codegen_.builder_.CreateICmpULE(codegen_.builder_.CreateSub(id, codegen_.builder_.getInt32(cls->id)),
                                                codegen_.builder_.getInt32(last - cls->id));
  codegen_.builder_.CreateCondBr(matches, callBlock, failBlock);
  codegen_.builder_.SetInsertPoint(failBlock);
  std::string message = className(cls) + "." + fn->name.str() +
                        " was called with a 'this' that is not an instance of " + className(cls);
  llvm::Value *text = codegen_.stringLowerer().stringLiteral(std::u16string(message.begin(), message.end()));
  codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_throw_str", codegen_.builder_.getVoidTy(),
                                      {codegen_.builder_.getInt32Ty(), ptr, codegen_.builder_.getInt64Ty()}),
                      {codegen_.builder_.getInt32(1), codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text)});
  for (size_t i = 1; i < params.size(); ++i) {
    codegen_.refCounts().releaseValue(adapter->getArg(unsigned(i + 1)), params[i]);
  }
  finish(ret->isVoid() ? nullptr : llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(ret)));
  codegen_.builder_.SetInsertPoint(callBlock);
  std::vector<llvm::Value *> args{self}; // borrowed, as a method takes its object
  for (unsigned i = 2; i < adapter->arg_size(); ++i) {
    args.push_back(adapter->getArg(i));
  }
  llvm::CallInst *result = codegen_.builder_.CreateCall(method, args);
  finish(ret->isVoid() ? nullptr : result);
  codegen_.function_ = outer;

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

} // namespace tz
