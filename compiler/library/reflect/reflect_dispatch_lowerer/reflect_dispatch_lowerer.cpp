#include "compiler/library/reflect/reflect_dispatch_lowerer/reflect_dispatch_lowerer.h"
#include "compiler/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
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

// What a class is called in messages: its name, or what it extends.
std::string className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

} // namespace

ReflectDispatchLowerer::ReflectDispatchLowerer(Lowering &lowering) : lowering_(lowering) {}

// (env, this: unknown, args: unknown[]) -> unknown: checks that the `this`
// is an object of the class that declares the variants highest up its
// chain, chooses the variant the arguments fit best (or throws a TypeError:
// none fits, or several equally well), and calls it. The callee owns its
// `this` and its arguments, as a function with a `this` parameter does.
llvm::Constant *ReflectDispatchLowerer::reflectDispatchInfo(
    const ClassDecl *cls, llvm::ArrayRef<const FunctionDecl *> variants, const Type *&type) {
  const Type *unknown = Type::unknown(), *list = Type::array(unknown);
  type = Type::function({unknown, list}, 1, /*hasRest=*/true, unknown, /*hasThis=*/true);
  llvm::StringRef name = variants.front()->name;
  llvm::Constant *&found = reflectDispatchers_[{cls, name.str()}];
  if (found) {
    return found;
  }
  std::string base =
      "tz." + lowering_.symbolPrefixes_.lookup(cls) + className(cls) + "." + name.str();
  auto *dispatch = llvm::Function::Create(lowering_.closureLowerer().closureFnType(type),
                                          llvm::GlobalValue::InternalLinkage, base + ".dispatch",
                                          lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(dispatch);
  llvm::Constant *info = llvm::ConstantStruct::get(
      lowering_.fnInfoTy_, {dispatch, lowering_.stringLowerer().stringLiteral(
                                          std::u16string(name.begin(), name.end()))});
  found =
      new llvm::GlobalVariable(*lowering_.module_, lowering_.fnInfoTy_, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, info, base + ".dispatch.info");

  llvm::IRBuilderBase::InsertPointGuard guard(lowering_.builder_);
  llvm::Function *outer = std::exchange(lowering_.function_, dispatch);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", dispatch));
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  llvm::Value *self = dispatch->getArg(1), *array = dispatch->getArg(2);
  auto release = [&] {
    lowering_.refCounts().releaseValue(array, list);
    lowering_.refCounts().releaseValue(self, unknown);
  };
  auto fail = [&](const std::string &message) {
    llvm::Value *text =
        lowering_.stringLowerer().stringLiteral(std::u16string(message.begin(), message.end()));
    lowering_.builder_.CreateCall(
        lowering_.unknownLowerer().unknownFunction("tz_throw_str", lowering_.builder_.getVoidTy(),
                                                   {i32, ptr, lowering_.builder_.getInt64Ty()}),
        {lowering_.builder_.getInt32(1), lowering_.stringLowerer().stringObj(text),
         lowering_.stringLowerer().stringMeta(text)});
    release();
    lowering_.builder_.CreateRet(null);
  };
  const ClassDecl *top = nullptr;
  for (const FunctionDecl *fn : variants) {
    if (!top || top->derivesFrom(fn->cls)) {
      top = fn->cls;
    }
  }
  std::string what = className(cls) + "." + name.str();
  // The `this`.
  llvm::BasicBlock *wrongBlock = lowering_.functionFrame().newBlock("this.wrong");
  llvm::BasicBlock *testBlock = lowering_.functionFrame().newBlock("this.test");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("this.ok");
  llvm::Value *layout = lowering_.builder_.CreateCall(
      lowering_.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {self},
      "layout");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(layout), testBlock,
                                  wrongBlock);
  lowering_.functionFrame().switchTo(testBlock);
  llvm::Value *id = lowering_.builder_.CreateLoad(
      i32, lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, layout, 6), "class");
  lowering_.builder_.CreateCondBr(lowering_.unknownCastLowerer().classIdIn(id, {top}), okBlock,
                                  wrongBlock);
  lowering_.functionFrame().switchTo(wrongBlock);
  fail(className(top) + "." + name.str() + " was called with a 'this' that is not an instance of " +
       className(top));
  // The variant.
  lowering_.functionFrame().switchTo(okBlock);
  llvm::Value *count =
      lowering_.builder_.CreateTrunc(lowering_.arrayLowerer().arrayLength(array), i32, "count");
  llvm::Value *args = lowering_.arrayLowerer().arrayData(array);
  llvm::Value *chosen = lowering_.reflectArgsLowerer().emitChooseVariant(variants, id, args, count);
  llvm::BasicBlock *noneBlock = lowering_.functionFrame().newBlock("variant.none");
  llvm::BasicBlock *severalBlock = lowering_.functionFrame().newBlock("variant.several");
  llvm::SwitchInst *which =
      lowering_.builder_.CreateSwitch(chosen, noneBlock, unsigned(variants.size() + 1));
  which->addCase(lowering_.builder_.getInt32(uint32_t(-2)), severalBlock);
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v]->sym;
    llvm::BasicBlock *callBlock = lowering_.functionFrame().newBlock("variant.call");
    which->addCase(lowering_.builder_.getInt32(uint32_t(v)), callBlock);
    lowering_.functionFrame().switchTo(callBlock);
    std::vector<const Type *> params = sym->params;
    const Type *rest = sym->hasRest ? params.back() : nullptr;
    if (rest) {
      params.pop_back();
    }
    std::vector<llvm::Value *> values{self}; // borrowed, as a method takes its object
    lowering_.reflectArgsLowerer().emitReflectArgs(args, count, sym->minArgs, params, rest, values,
                                                   [&](llvm::Value *, const Type *, llvm::Value *) {
                                                     fail("Reflect.apply: no variant of " + what +
                                                          " takes these arguments");
                                                   });
    llvm::Value *result =
        lowering_.builder_.CreateCall(lowering_.classDescriptors().methodCode(variants[v]), values);
    llvm::BasicBlock *thrownBlock = lowering_.functionFrame().newBlock("variant.thrown");
    llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("variant.done");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateIsNotNull(
            lowering_.builder_.CreateLoad(ptr, lowering_.throwLowerer().thrownSlot())),
        thrownBlock, doneBlock, llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
    lowering_.functionFrame().switchTo(thrownBlock);
    release();
    lowering_.builder_.CreateRet(null);
    lowering_.functionFrame().switchTo(doneBlock);
    llvm::Value *value =
        sym->ret->isVoid()
            ? null
            : lowering_.exprLowerer().emitConversion(result, sym->ret, Type::unknown());
    release();
    lowering_.builder_.CreateRet(value);
  }
  lowering_.functionFrame().switchTo(noneBlock);
  fail("Reflect.apply: no variant of " + what + " takes these arguments");
  lowering_.functionFrame().switchTo(severalBlock);
  fail("Reflect.apply: the arguments fit several variants of " + what + " equally well");
  lowering_.function_ = outer;

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
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  fn = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr, ptr, ptr, i32}, false),
                              llvm::GlobalValue::InternalLinkage, "tz.invoke",
                              lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPointGuard guard(lowering_.builder_);
  llvm::Function *outer = std::exchange(lowering_.function_, fn);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
  llvm::Value *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  llvm::Value *value = lowering_.builder_.CreateLoad(lowering_.fnTy_, fn->getArg(0), "fn");
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
  llvm::Value *fits =
      lowering_.builder_.CreateICmpUGE(count, lowering_.builder_.getInt32(uint32_t(min)));
  if (!rest) {
    fits = lowering_.builder_.CreateAnd(
        fits, lowering_.builder_.CreateICmpULE(
                  count, lowering_.builder_.getInt32(uint32_t(params.size()))));
  }
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("reflect.arity");
  llvm::BasicBlock *wrongBlock = lowering_.functionFrame().newBlock("reflect.arity.wrong");
  lowering_.builder_.CreateCondBr(fits, okBlock, wrongBlock);
  lowering_.functionFrame().switchTo(wrongBlock);
  lowering_.builder_.CreateCall(
      lowering_.unknownLowerer().unknownFunction("tz_reflect_arity", lowering_.builder_.getVoidTy(),
                                                 {ptr, i32, i32, i32, i32}),
      {lowering_.stringLowerer().utf8Constant(what),
       lowering_.builder_.getInt32(uint32_t(sizeof what - 1)),
       lowering_.builder_.getInt32(uint32_t(min)),
       lowering_.builder_.getInt32(rest ? -1 : int32_t(params.size())), count});
  lowering_.builder_.CreateRet(null);
  lowering_.functionFrame().switchTo(okBlock);
  std::vector<llvm::Value *> values;
  if (first) {
    lowering_.refCounts().retainValue(self, Type::unknown()); // the callee owns its `this`
    values.push_back(self);
  }
  lowering_.reflectArgsLowerer().emitReflectArgs(
      args, count, min, params, rest, values,
      [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
        if (first) {
          lowering_.refCounts().releaseValue(self, Type::unknown());
        }
        lowering_.reflectArgsLowerer().emitReflectMismatch(what, i, expected, got);
        lowering_.builder_.CreateRet(null);
      });
  llvm::Value *result = lowering_.closureLowerer().callFunctionValue(value, fnType, values);
  llvm::BasicBlock *thrownBlock = lowering_.functionFrame().newBlock("reflect.thrown");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("reflect.done");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(lowering_.builder_.CreateLoad(
                                      ptr, lowering_.throwLowerer().thrownSlot())),
                                  thrownBlock, doneBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(thrownBlock);
  lowering_.builder_.CreateRet(null);
  lowering_.functionFrame().switchTo(doneBlock);
  const Type *ret = fnType->ret();
  lowering_.builder_.CreateRet(ret->isVoid() || !result ? null
                                                        : lowering_.exprLowerer().emitConversion(
                                                              result, ret, Type::unknown()));
  lowering_.function_ = outer;

  return fn;
}

} // namespace tz
