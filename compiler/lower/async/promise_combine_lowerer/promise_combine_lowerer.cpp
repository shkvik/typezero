#include "compiler/lower/async/promise_combine_lowerer/promise_combine_lowerer.h"
#include "compiler/check/checker/checker.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

enum CombineKind : int32_t { kAll, kRace };

}

PromiseCombineLowerer::PromiseCombineLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *PromiseCombineLowerer::emitPromiseCombine(CallExpr *call) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  const Type *anyPromise = Type::promise(Type::voidTy());
  Expr *arg = call->args[0];
  const Type *argType = arg->type;
  auto toPromise = [&](llvm::Value *value, const Type *type) -> llvm::Value * {
    if (type->isPromise()) {
      return value;
    }
    auto *fn = cast<llvm::Function>(
        this->lowering.asyncLowerer().asyncFunction("tz_promise_resolved", ptr, {ptr}).getCallee());
    llvm::Value *promise = this->lowering.builder_.CreateCall(
        fn, {this->lowering.promiseLowerer().promiseLayout(type)}, "promise");
    if (this->lowering.promiseLowerer().promiseValueSize(type)) {
      this->lowering.builder_.CreateStore(
          value, this->lowering.promiseLowerer().promiseValueAddress(promise));
    }

    return promise;
  };
  auto arrayOf = [&](std::vector<llvm::Value *> values, llvm::ArrayRef<const Type *> types) {
    llvm::Value *array = this->lowering.builder_.CreateCall(
        this->lowering.rt_.arrNew,
        {this->lowering.typeDescriptors().typeDescriptor(anyPromise),
         this->lowering.builder_.getInt64(values.size()),
         this->lowering.builder_.getInt64(values.size())},
        "promises");
    llvm::Value *data = this->lowering.arrayLowerer().arrayData(array);
    for (size_t i = 0; i < values.size(); ++i) {
      this->lowering.builder_.CreateStore(
          toPromise(values[i], types[i]),
          this->lowering.builder_.CreateConstInBoundsGEP1_64(ptr, data, i));
    }

    return array;
  };
  llvm::Value *promises;
  std::vector<const Type *> given;
  auto *lit = dyn_cast<ArrayLitExpr>(arg);
  if (lit && llvm::any_of(lit->elements,
                          [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; })) {
    lit = nullptr;
  }
  if (lit) {
    std::vector<Expr *> exprs;
    std::vector<const Type *> types;
    for (ArrayLitExpr::Element &e : lit->elements) {
      exprs.push_back(e.expr);
      types.push_back(e.expr->type);
    }
    if (argType->isTuple()) {
      given = types;
    }
    promises = arrayOf(this->lowering.exprLowerer().emitExprs(exprs), types);
  } else if (argType->isTuple()) {
    llvm::Value *tuple = this->lowering.exprLowerer().emitExpr(arg);
    std::vector<llvm::Value *> values;
    std::vector<const Type *> types;
    for (const Field &f : argType->fields()) {
      given.push_back(f.type);
    }
    for (const Field &f : argType->fields()) {
      llvm::Value *v = this->lowering.builder_.CreateLoad(
          this->lowering.moduleBuilder().lower(f.type),
          this->lowering.objectLayouts().fieldAddress(tuple, argType, f.name));
      this->lowering.refCounts().retainValue(v, f.type);
      values.push_back(v);
      types.push_back(f.type);
    }
    this->lowering.refCounts().releaseValue(tuple, argType);
    promises = arrayOf(values, types);
  } else if (argType->inner()->isPromise()) {
    promises = this->lowering.exprLowerer().emitExpr(arg);
  } else {
    const Type *element = argType->inner();
    llvm::Value *values = this->lowering.exprLowerer().emitExpr(arg);
    promises = this->lowering.builder_.CreateCall(
        this->lowering.asyncLowerer().asyncFunction("tz_promise_resolve_all", ptr, {ptr, ptr, ptr}),
        {values, this->lowering.promiseLowerer().promiseLayout(element),
         this->lowering.typeDescriptors().typeDescriptor(anyPromise)},
        "promises");
    this->lowering.refCounts().releaseValue(values, argType);
  }

  const Type *result = call->type->promised();
  int32_t kind = call->builtin() == Builtin::PromiseAll ? kAll : kRace;
  llvm::Constant *null = llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  llvm::Constant *items = null;
  llvm::Constant *tuple = null;
  llvm::Constant *convert = null;
  uint32_t tupleSize = 0;
  const llvm::DataLayout &dl = this->lowering.module_->getDataLayout();
  if (result->isTuple()) {
    std::vector<unsigned> keys(result->fields().size());
    for (unsigned i = 0; i < keys.size(); ++i) {
      keys[i] = i;
    }
    tuple = this->lowering.objectLayouts().layoutDescriptor(result, keys);
    tupleSize =
        uint32_t(dl.getTypeAllocSize(this->lowering.objectLayouts().objectLayout(result).type));
  } else if (result->isArray()) {
    items = this->lowering.typeDescriptors().typeDescriptor(result->inner());
  }
  if (kind == kRace && this->lowering.promiseLowerer().promiseValueSize(result)) {
    std::vector<llvm::Constant *> fns;
    bool converts = false;
    for (const Type *t : given) {
      const Type *value = t->isPromise() ? t->promised() : t;
      if (value == result || value->isNever() ||
          !this->lowering.promiseLowerer().promiseValueSize(value)) {
        fns.push_back(null);
        continue;
      }
      fns.push_back(this->combineConverter(value, result));
      converts = true;
    }
    if (converts) {
      auto *fnsType = llvm::ArrayType::get(ptr, fns.size());
      auto *global = new llvm::GlobalVariable(
          *this->lowering.module_, fnsType, true, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantArray::get(fnsType, fns), "tz.promise.convert");
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      convert = global;
    }
  }
  auto *shapeType = llvm::StructType::get(this->lowering.ctx_, {ptr, ptr, ptr, i32, ptr});
  auto *shape = new llvm::GlobalVariable(
      *this->lowering.module_, shapeType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(shapeType,
                                {this->lowering.promiseLowerer().promiseLayout(result), items,
                                 tuple, this->lowering.builder_.getInt32(tupleSize), convert}),
      "tz.promise.combine");
  shape->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::Value *promise = this->lowering.builder_.CreateCall(
      this->lowering.asyncLowerer().asyncFunction("tz_promise_combine", ptr, {i32, ptr, ptr}),
      {this->lowering.builder_.getInt32(uint32_t(kind)), promises, shape}, "promise");
  this->lowering.builder_.CreateCall(
      this->lowering.memory().hasCycles
          ? this->lowering.groupLowerer().groupAwareRelease(this->lowering.arrReleaseFn_)
          : this->lowering.arrReleaseFn_,
      {promises});

  return promise;
}

llvm::Function *PromiseCombineLowerer::combineConverter(const Type *from, const Type *to) {
  llvm::Function *&fn = this->combineConverters[{from, to}];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  fn = llvm::Function::Create(
      llvm::FunctionType::get(this->lowering.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.convert", this->lowering.module_.get());
  this->lowering.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
  llvm::Function *outer = std::exchange(this->lowering.function_, fn);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", fn));
  llvm::Value *value = this->lowering.builder_.CreateLoad(
      this->lowering.moduleBuilder().lower(from), fn->getArg(1), "value");
  this->lowering.refCounts().retainValue(value, from);
  this->lowering.builder_.CreateStore(this->lowering.exprLowerer().emitConversion(value, from, to),
                                      fn->getArg(0));
  this->lowering.builder_.CreateRetVoid();
  this->lowering.function_ = outer;
  this->lowering.builder_.restoreIP(point);

  return fn;
}

}
