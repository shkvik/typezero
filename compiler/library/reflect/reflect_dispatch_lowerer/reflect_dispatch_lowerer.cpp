#include "compiler/library/reflect/reflect_dispatch_lowerer/reflect_dispatch_lowerer.h"
#include "compiler/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
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


namespace tz {

std::string ReflectDispatchLowerer::className(const ClassDecl *cls) {
  return cls->name.empty() && cls->type ? cls->type->name().str() : cls->name.str();
}

ReflectDispatchLowerer::ReflectDispatchLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Constant *ReflectDispatchLowerer::reflectDispatchInfo(
    const ClassDecl *cls, llvm::ArrayRef<const FunctionDecl *> variants, const Type *&type) {
  const Type *unknown = Type::unknown();
  const Type *list = Type::array(unknown);
  type = Type::function({unknown, list}, 1, true, unknown, true);
  llvm::StringRef name = variants.front()->name;
  llvm::Constant *&found = this->reflectDispatchers[{cls, name.str()}];
  if (found) {
    return found;
  }
  std::string base = "tz." + this->lowering.symbolPrefixes_.lookup(cls) +
                     ReflectDispatchLowerer::className(cls) + "." + name.str();
  auto *dispatch = llvm::Function::Create(this->lowering.closureLowerer().closureFnType(type),
                                          llvm::GlobalValue::InternalLinkage, base + ".dispatch",
                                          this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(dispatch);
  llvm::Constant *info = llvm::ConstantStruct::get(
      this->lowering.fnInfoTy_, {dispatch, this->lowering.stringLowerer().stringLiteral(
                                               std::u16string(name.begin(), name.end()))});
  found =
      new llvm::GlobalVariable(*this->lowering.module_, this->lowering.fnInfoTy_, true,
                               llvm::GlobalValue::PrivateLinkage, info, base + ".dispatch.info");

  llvm::IRBuilderBase::InsertPointGuard guard(this->lowering.builder_);
  llvm::Function *outer = std::exchange(this->lowering.function_, dispatch);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", dispatch));
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  llvm::Value *self = dispatch->getArg(1);
  llvm::Value *array = dispatch->getArg(2);
  auto release = [&] {
    this->lowering.refCounts().releaseValue(array, list);
    this->lowering.refCounts().releaseValue(self, unknown);
  };
  auto fail = [&](llvm::StringRef code, const std::string &message) {
    this->lowering.reflectArgsLowerer().emitReflectFail(code, message);
    release();
    this->lowering.builder_.CreateRet(null);
  };
  const ClassDecl *top = nullptr;
  for (const FunctionDecl *fn : variants) {
    if (!top || top->derivesFrom(fn->cls())) {
      top = fn->cls();
    }
  }
  std::string what = ReflectDispatchLowerer::className(cls) + "." + name.str();
  llvm::BasicBlock *wrongBlock = this->lowering.functionFrame().newBlock("this.wrong");
  llvm::BasicBlock *testBlock = this->lowering.functionFrame().newBlock("this.test");
  llvm::BasicBlock *okBlock = this->lowering.functionFrame().newBlock("this.ok");
  llvm::Value *layout = this->lowering.builder_.CreateCall(
      this->lowering.unknownLowerer().unknownFunction("tz_unknown_layout", ptr, {ptr}), {self},
      "layout");
  this->lowering.builder_.CreateCondBr(this->lowering.builder_.CreateIsNotNull(layout), testBlock,
                                       wrongBlock);
  this->lowering.functionFrame().switchTo(testBlock);
  llvm::Value *id = this->lowering.builder_.CreateLoad(
      i32, this->lowering.builder_.CreateStructGEP(this->lowering.layoutDescTy_, layout, 6),
      "class");
  this->lowering.builder_.CreateCondBr(this->lowering.unknownCastLowerer().classIdIn(id, {top}),
                                       okBlock, wrongBlock);
  this->lowering.functionFrame().switchTo(wrongBlock);
  fail("ERR_INVALID_THIS", ReflectDispatchLowerer::className(top) + "." + name.str() +
                               " was called with a 'this' that is not an instance of " +
                               ReflectDispatchLowerer::className(top));
  this->lowering.functionFrame().switchTo(okBlock);
  llvm::Value *count = this->lowering.builder_.CreateTrunc(
      this->lowering.arrayLowerer().arrayLength(array), i32, "count");
  llvm::Value *args = this->lowering.arrayLowerer().arrayData(array);
  llvm::Value *chosen =
      this->lowering.reflectArgsLowerer().emitChooseVariant(variants, id, args, count);
  llvm::BasicBlock *noneBlock = this->lowering.functionFrame().newBlock("variant.none");
  llvm::BasicBlock *severalBlock = this->lowering.functionFrame().newBlock("variant.several");
  llvm::SwitchInst *which =
      this->lowering.builder_.CreateSwitch(chosen, noneBlock, unsigned(variants.size() + 1));
  which->addCase(this->lowering.builder_.getInt32(uint32_t(-2)), severalBlock);
  for (size_t v = 0; v < variants.size(); ++v) {
    const FuncSymbol *sym = variants[v]->sym;
    llvm::BasicBlock *callBlock = this->lowering.functionFrame().newBlock("variant.call");
    which->addCase(this->lowering.builder_.getInt32(uint32_t(v)), callBlock);
    this->lowering.functionFrame().switchTo(callBlock);
    std::vector<const Type *> params = sym->params;
    const Type *rest = sym->hasRest ? params.back() : nullptr;
    if (rest) {
      params.pop_back();
    }
    std::vector<llvm::Value *> values{self};
    this->lowering.reflectArgsLowerer().emitReflectArgs(
        args, count, sym->minArgs, params, rest, values,
        [&](llvm::Value *, const Type *, llvm::Value *) {
          fail("ERR_NO_VARIANT", "Reflect.apply: no variant of " + what + " takes these arguments");
        });
    llvm::Value *result = this->lowering.builder_.CreateCall(
        this->lowering.classDescriptors().methodCode(variants[v]), values);
    llvm::Value *value =
        sym->ret->isVoid()
            ? null
            : this->lowering.exprLowerer().emitConversion(result, sym->ret, Type::unknown());
    release();
    this->lowering.builder_.CreateRet(value);
  }
  this->lowering.functionFrame().switchTo(noneBlock);
  fail("ERR_NO_VARIANT", "Reflect.apply: no variant of " + what + " takes these arguments");
  this->lowering.functionFrame().switchTo(severalBlock);
  fail("ERR_SEVERAL_VARIANTS",
       "Reflect.apply: the arguments fit several variants of " + what + " equally well");
  this->lowering.function_ = outer;

  return found;
}

llvm::Function *ReflectDispatchLowerer::invokerFor(const Type *fnType) {
  llvm::Function *&fn = this->invokers[fnType];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  fn = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr, ptr, ptr, i32}, false),
                              llvm::GlobalValue::InternalLinkage, "tz.invoke",
                              this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPointGuard guard(this->lowering.builder_);
  llvm::Function *outer = std::exchange(this->lowering.function_, fn);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", fn));
  llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  llvm::Value *value =
      this->lowering.builder_.CreateLoad(this->lowering.fnTy_, fn->getArg(0), "fn");
  llvm::Value *self = fn->getArg(1);
  llvm::Value *args = fn->getArg(2);
  llvm::Value *count = fn->getArg(3);
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
      this->lowering.builder_.CreateICmpUGE(count, this->lowering.builder_.getInt32(uint32_t(min)));
  if (!rest) {
    fits = this->lowering.builder_.CreateAnd(
        fits, this->lowering.builder_.CreateICmpULE(
                  count, this->lowering.builder_.getInt32(uint32_t(params.size()))));
  }
  llvm::BasicBlock *okBlock = this->lowering.functionFrame().newBlock("reflect.arity");
  llvm::BasicBlock *wrongBlock = this->lowering.functionFrame().newBlock("reflect.arity.wrong");
  this->lowering.builder_.CreateCondBr(fits, okBlock, wrongBlock);
  this->lowering.functionFrame().switchTo(wrongBlock);
  this->lowering.builder_.CreateCall(
      this->lowering.unknownLowerer().unknownFunction(
          "tz_reflect_arity", this->lowering.builder_.getVoidTy(), {ptr, i32, i32, i32, i32}),
      {this->lowering.stringLowerer().utf8Constant(what),
       this->lowering.builder_.getInt32(uint32_t(sizeof what - 1)),
       this->lowering.builder_.getInt32(uint32_t(min)),
       this->lowering.builder_.getInt32(rest ? -1 : int32_t(params.size())), count});
  this->lowering.builder_.CreateRet(null);
  this->lowering.functionFrame().switchTo(okBlock);
  std::vector<llvm::Value *> values;
  if (first) {
    this->lowering.refCounts().retainValue(self, Type::unknown());
    values.push_back(self);
  }
  this->lowering.reflectArgsLowerer().emitReflectArgs(
      args, count, min, params, rest, values,
      [&](llvm::Value *i, const Type *expected, llvm::Value *got) {
        if (first) {
          this->lowering.refCounts().releaseValue(self, Type::unknown());
        }
        this->lowering.reflectArgsLowerer().emitReflectMismatch(what, i, expected, got);
        this->lowering.builder_.CreateRet(null);
      });
  llvm::Value *result = this->lowering.closureLowerer().callFunctionValue(value, fnType, values);
  const Type *ret = fnType->ret();
  this->lowering.builder_.CreateRet(
      ret->isVoid() || !result
          ? null
          : this->lowering.exprLowerer().emitConversion(result, ret, Type::unknown()));
  this->lowering.function_ = outer;

  return fn;
}

}
