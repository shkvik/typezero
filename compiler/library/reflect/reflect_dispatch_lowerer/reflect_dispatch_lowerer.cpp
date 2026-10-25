#include "compiler/library/reflect/reflect_dispatch_lowerer/reflect_dispatch_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
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

} // namespace

ReflectDispatchLowerer::ReflectDispatchLowerer(CodeGen &codegen) : codegen_(codegen) {}

// (env, this: unknown, args: unknown[]) -> unknown: checks that the `this`
// is an object of the class that declares the variants highest up its
// chain, chooses the variant the arguments fit best (or throws a TypeError:
// none fits, or several equally well), and calls it. The callee owns its
// `this` and its arguments, as a function with a `this` parameter does.
llvm::Constant *ReflectDispatchLowerer::reflectDispatchInfo(const ClassDecl *cls,
                                             llvm::ArrayRef<const FunctionDecl *> variants,
                                             const Type *&type) {
  const Type *unknown = Type::unknown(), *list = Type::array(unknown);
  type = Type::function({unknown, list}, 1, /*hasRest=*/true, unknown, /*hasThis=*/true);
  llvm::StringRef name = variants.front()->name;
  llvm::Constant *&found = reflectDispatchers_[{cls, name.str()}];
  if (found) {
    return found;
  }
  std::string base = "tz." + codegen_.symbolPrefixes_.lookup(cls) + className(cls) + "." + name.str();
  auto *dispatch = llvm::Function::Create(codegen_.closureLowerer().closureFnType(type), llvm::GlobalValue::InternalLinkage,
                                          base + ".dispatch", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(dispatch);
  llvm::Constant *info = llvm::ConstantStruct::get(
      codegen_.fnInfoTy_, {dispatch, codegen_.stringLowerer().stringLiteral(std::u16string(name.begin(), name.end()))});
  found =
      new llvm::GlobalVariable(*codegen_.module_, codegen_.fnInfoTy_, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, info, base + ".dispatch.info");

  llvm::IRBuilderBase::InsertPointGuard guard(codegen_.builder_);
  llvm::Function *outer = std::exchange(codegen_.function_, dispatch);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", dispatch));
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  llvm::Value *self = dispatch->getArg(1), *array = dispatch->getArg(2);
  auto release = [&] {
    codegen_.refCounts().releaseValue(array, list);
    codegen_.refCounts().releaseValue(self, unknown);
  };
  auto fail = [&](const std::string &message) {
    llvm::Value *text = codegen_.stringLowerer().stringLiteral(std::u16string(message.begin(), message.end()));
    codegen_.builder_.CreateCall(
        codegen_.unknownLowerer().unknownFunction("tz_throw_str", codegen_.builder_.getVoidTy(), {i32, ptr, codegen_.builder_.getInt64Ty()}),
        {codegen_.builder_.getInt32(1), codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text)});
    release();
    codegen_.builder_.CreateRet(null);
  };
  const ClassDecl *top = nullptr;
  for (const FunctionDecl *fn : variants) {
    if (!top || top->derivesFrom(fn->cls)) {
      top = fn->cls;
    }
  }
  std::string what = className(cls) + "." + name.str();
  // The `this`.
  llvm::BasicBlock *wrongBlock = codegen_.functionFrame().newBlock("this.wrong");
  llvm::BasicBlock *testBlock = codegen_.functionFrame().newBlock("this.test");
  llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("this.ok");
  llvm::Value *layout =
      codegen_.builder_.CreateCall(codegen_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {self}, "layout");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(layout), testBlock, wrongBlock);
  codegen_.functionFrame().switchTo(testBlock);
  llvm::Value *id =
      codegen_.builder_.CreateLoad(i32, codegen_.builder_.CreateStructGEP(codegen_.layoutDescTy_, layout, 6), "class");
  codegen_.builder_.CreateCondBr(codegen_.unknownCastLowerer().classIdIn(id, {top}), okBlock, wrongBlock);
  codegen_.functionFrame().switchTo(wrongBlock);
  fail(className(top) + "." + name.str() + " was called with a 'this' that is not an instance of " +
       className(top));
  // The variant.
  codegen_.functionFrame().switchTo(okBlock);
  llvm::Value *count = codegen_.builder_.CreateTrunc(codegen_.arrayLowerer().arrayLength(array), i32, "count");
  llvm::Value *args = codegen_.arrayLowerer().arrayData(array);
  llvm::Value *chosen = codegen_.reflectArgsLowerer().emitChooseVariant(variants, id, args, count);
  llvm::BasicBlock *noneBlock = codegen_.functionFrame().newBlock("variant.none");
  llvm::BasicBlock *severalBlock = codegen_.functionFrame().newBlock("variant.several");
  llvm::SwitchInst *which = codegen_.builder_.CreateSwitch(chosen, noneBlock, unsigned(variants.size() + 1));
  which->addCase(codegen_.builder_.getInt32(uint32_t(-2)), severalBlock);
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v]->sym;
    llvm::BasicBlock *callBlock = codegen_.functionFrame().newBlock("variant.call");
    which->addCase(codegen_.builder_.getInt32(uint32_t(v)), callBlock);
    codegen_.functionFrame().switchTo(callBlock);
    std::vector<const Type *> params = sym->params;
    const Type *rest = sym->hasRest ? params.back() : nullptr;
    if (rest) {
      params.pop_back();
    }
    std::vector<llvm::Value *> values{self}; // borrowed, as a method takes its object
    codegen_.reflectArgsLowerer().emitReflectArgs(args, count, sym->minArgs, params, rest, values,
                    [&](llvm::Value *, const Type *, llvm::Value *) {
                      fail("Reflect.apply: no variant of " + what + " takes these arguments");
                    });
    llvm::Value *result = codegen_.builder_.CreateCall(codegen_.classDescriptors().methodCode(variants[v]), values);
    llvm::BasicBlock *thrownBlock = codegen_.functionFrame().newBlock("variant.thrown");
    llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("variant.done");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot())),
                          thrownBlock, doneBlock,
                          llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
    codegen_.functionFrame().switchTo(thrownBlock);
    release();
    codegen_.builder_.CreateRet(null);
    codegen_.functionFrame().switchTo(doneBlock);
    llvm::Value *value =
        sym->ret->isVoid() ? null : codegen_.exprLowerer().emitConversion(result, sym->ret, Type::unknown());
    release();
    codegen_.builder_.CreateRet(value);
  }
  codegen_.functionFrame().switchTo(noneBlock);
  fail("Reflect.apply: no variant of " + what + " takes these arguments");
  codegen_.functionFrame().switchTo(severalBlock);
  fail("Reflect.apply: the arguments fit several variants of " + what + " equally well");
  codegen_.function_ = outer;

  return found;
}

// (ptr f, ptr self, ptr args, i32 count) -> ptr: calls the function value
// `*f` of `fnType` with arguments as unknowns (borrowed), and `self` for a
// `this` parameter; the result as an unknown, NULL after throwing.
llvm::Function *ReflectDispatchLowerer::invokerFor(const Type *fnType) {
  llvm::Function *&fn = invokers_[fnType];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  fn = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr, ptr, ptr, i32}, false),
                              llvm::GlobalValue::InternalLinkage, "tz.invoke", codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPointGuard guard(codegen_.builder_);
  llvm::Function *outer = std::exchange(codegen_.function_, fn);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
  llvm::Value *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  llvm::Value *value = codegen_.builder_.CreateLoad(codegen_.fnTy_, fn->getArg(0), "fn");
  llvm::Value *self = fn->getArg(1), *args = fn->getArg(2), *count = fn->getArg(3);
  size_t first = fnType->hasThis() ? 1 : 0;
  std::vector<const Type *> params(fnType->params().begin() + first, fnType->params().end());
  const Type *rest = nullptr;
  if (fnType->hasRest()) {
    rest = params.back();
    params.pop_back();
  }
  size_t min = fnType->minArgs() > first ? fnType->minArgs() - first : 0;
  const char what[] = "Reflect.apply";
  llvm::Value *fits = codegen_.builder_.CreateICmpUGE(count, codegen_.builder_.getInt32(uint32_t(min)));
  if (!rest) {
    fits = codegen_.builder_.CreateAnd(
        fits, codegen_.builder_.CreateICmpULE(count, codegen_.builder_.getInt32(uint32_t(params.size()))));
  }
  llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("reflect.arity");
  llvm::BasicBlock *wrongBlock = codegen_.functionFrame().newBlock("reflect.arity.wrong");
  codegen_.builder_.CreateCondBr(fits, okBlock, wrongBlock);
  codegen_.functionFrame().switchTo(wrongBlock);
  codegen_.builder_.CreateCall(
      codegen_.unknownLowerer().unknownFunction("tz_reflect_arity", codegen_.builder_.getVoidTy(), {ptr, i32, i32, i32, i32}),
      {codegen_.stringLowerer().utf8Constant(what), codegen_.builder_.getInt32(uint32_t(sizeof what - 1)),
       codegen_.builder_.getInt32(uint32_t(min)), codegen_.builder_.getInt32(rest ? -1 : int32_t(params.size())),
       count});
  codegen_.builder_.CreateRet(null);
  codegen_.functionFrame().switchTo(okBlock);
  std::vector<llvm::Value *> values;
  if (first) {
    codegen_.refCounts().retainValue(self, Type::unknown()); // the callee owns its `this`
    values.push_back(self);
  }
  codegen_.reflectArgsLowerer().emitReflectArgs(args, count, min, params, rest, values,
                  [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
                    if (first) {
                      codegen_.refCounts().releaseValue(self, Type::unknown());
                    }
                    codegen_.reflectArgsLowerer().emitReflectMismatch(what, i, expected, got);
                    codegen_.builder_.CreateRet(null);
                  });
  llvm::Value *result = codegen_.closureLowerer().callFunctionValue(value, fnType, values);
  llvm::BasicBlock *thrownBlock = codegen_.functionFrame().newBlock("reflect.thrown");
  llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("reflect.done");
  codegen_.builder_.CreateCondBr(codegen_.builder_.CreateIsNotNull(codegen_.builder_.CreateLoad(ptr, codegen_.throwLowerer().thrownSlot())),
                        thrownBlock, doneBlock,
                        llvm::MDBuilder(codegen_.ctx_).createUnlikelyBranchWeights());
  codegen_.functionFrame().switchTo(thrownBlock);
  codegen_.builder_.CreateRet(null);
  codegen_.functionFrame().switchTo(doneBlock);
  const Type *ret = fnType->ret();
  codegen_.builder_.CreateRet(ret->isVoid() || !result ? null
                                              : codegen_.exprLowerer().emitConversion(result, ret, Type::unknown()));
  codegen_.function_ = outer;

  return fn;
}

} // namespace tz
