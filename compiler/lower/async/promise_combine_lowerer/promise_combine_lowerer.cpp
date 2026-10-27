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

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

enum CombineKind : int32_t { kAll, kRace };

}

PromiseCombineLowerer::PromiseCombineLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *PromiseCombineLowerer::emitPromiseCombine(CallExpr *call) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  const Type *anyPromise = Type::promise(Type::voidTy());
  Expr *arg = call->args[0];
  const Type *argType = arg->type;
  auto toPromise = [&](llvm::Value *value, const Type *type) -> llvm::Value * {
    if (type->isPromise()) {
      return value;
    }
    auto *fn = cast<llvm::Function>(
        lowering_.asyncLowerer().asyncFunction("tz_promise_resolved", ptr, {ptr}).getCallee());
    llvm::Value *promise = lowering_.builder_.CreateCall(
        fn, {lowering_.promiseLowerer().promiseLayout(type)}, "promise");
    if (lowering_.promiseLowerer().promiseValueSize(type)) {
      lowering_.builder_.CreateStore(value,
                                     lowering_.promiseLowerer().promiseValueAddress(promise));
    }

    return promise;
  };
  auto arrayOf = [&](std::vector<llvm::Value *> values, llvm::ArrayRef<const Type *> types) {
    llvm::Value *array = lowering_.builder_.CreateCall(
        lowering_.rt_.arrNew,
        {lowering_.typeDescriptors().typeDescriptor(anyPromise),
         lowering_.builder_.getInt64(values.size()), lowering_.builder_.getInt64(values.size())},
        "promises");
    llvm::Value *data = lowering_.arrayLowerer().arrayData(array);
    for (size_t i = 0; i < values.size(); ++i) {
      lowering_.builder_.CreateStore(toPromise(values[i], types[i]),
                                     lowering_.builder_.CreateConstInBoundsGEP1_64(ptr, data, i));
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
    promises = arrayOf(lowering_.exprLowerer().emitExprs(exprs), types);
  } else if (argType->isTuple()) {
    llvm::Value *tuple = lowering_.exprLowerer().emitExpr(arg);
    std::vector<llvm::Value *> values;
    std::vector<const Type *> types;
    for (const Field &f : argType->fields()) {
      given.push_back(f.type);
    }
    for (const Field &f : argType->fields()) {
      llvm::Value *v = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(f.type),
          lowering_.objectLayouts().fieldAddress(tuple, argType, f.name));
      lowering_.refCounts().retainValue(v, f.type);
      values.push_back(v);
      types.push_back(f.type);
    }
    lowering_.refCounts().releaseValue(tuple, argType);
    promises = arrayOf(values, types);
  } else if (argType->inner()->isPromise()) {
    promises = lowering_.exprLowerer().emitExpr(arg);
  } else {
    const Type *element = argType->inner();
    llvm::Value *values = lowering_.exprLowerer().emitExpr(arg);
    promises = lowering_.builder_.CreateCall(
        lowering_.asyncLowerer().asyncFunction("tz_promise_resolve_all", ptr, {ptr, ptr, ptr}),
        {values, lowering_.promiseLowerer().promiseLayout(element),
         lowering_.typeDescriptors().typeDescriptor(anyPromise)},
        "promises");
    lowering_.refCounts().releaseValue(values, argType);
  }

  const Type *result = call->type->promised();
  int32_t kind = call->builtin == Builtin::PromiseAll ? kAll : kRace;
  llvm::Constant *null = llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  llvm::Constant *items = null, *tuple = null, *convert = null;
  uint32_t tupleSize = 0;
  const llvm::DataLayout &dl = lowering_.module_->getDataLayout();
  if (result->isTuple()) {
    std::vector<unsigned> keys(result->fields().size());
    for (unsigned i = 0; i < keys.size(); ++i) {
      keys[i] = i;
    }
    tuple = lowering_.objectLayouts().layoutDescriptor(result, keys);
    tupleSize = uint32_t(dl.getTypeAllocSize(lowering_.objectLayouts().objectLayout(result).type));
  } else if (result->isArray()) {
    items = lowering_.typeDescriptors().typeDescriptor(result->inner());
  }
  if (kind == kRace && lowering_.promiseLowerer().promiseValueSize(result)) {
    std::vector<llvm::Constant *> fns;
    bool converts = false;
    for (const Type *t : given) {
      const Type *value = t->isPromise() ? t->promised() : t;
      if (value == result || value->isNever() ||
          !lowering_.promiseLowerer().promiseValueSize(value)) {
        fns.push_back(null);
        continue;
      }
      fns.push_back(combineConverter(value, result));
      converts = true;
    }
    if (converts) {
      auto *fnsType = llvm::ArrayType::get(ptr, fns.size());
      auto *global = new llvm::GlobalVariable(
          *lowering_.module_, fnsType, true, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantArray::get(fnsType, fns), "tz.promise.convert");
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      convert = global;
    }
  }
  auto *shapeType = llvm::StructType::get(lowering_.ctx_, {ptr, ptr, ptr, i32, ptr});
  auto *shape = new llvm::GlobalVariable(
      *lowering_.module_, shapeType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(shapeType,
                                {lowering_.promiseLowerer().promiseLayout(result), items, tuple,
                                 lowering_.builder_.getInt32(tupleSize), convert}),
      "tz.promise.combine");
  shape->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::Value *promise = lowering_.builder_.CreateCall(
      lowering_.asyncLowerer().asyncFunction("tz_promise_combine", ptr, {i32, ptr, ptr}),
      {lowering_.builder_.getInt32(uint32_t(kind)), promises, shape}, "promise");
  lowering_.builder_.CreateCall(
      lowering_.memory().hasCycles
          ? lowering_.groupLowerer().groupAwareRelease(lowering_.arrReleaseFn_)
          : lowering_.arrReleaseFn_,
      {promises});

  return promise;
}

llvm::Function *PromiseCombineLowerer::combineConverter(const Type *from, const Type *to) {
  llvm::Function *&fn = combineConverters_[{from, to}];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  fn = llvm::Function::Create(
      llvm::FunctionType::get(lowering_.builder_.getVoidTy(), {ptr, ptr}, false),
      llvm::GlobalValue::InternalLinkage, "tz.promise.convert", lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, fn);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", fn));
  llvm::Value *value =
      lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(from), fn->getArg(1), "value");
  lowering_.refCounts().retainValue(value, from);
  lowering_.builder_.CreateStore(lowering_.exprLowerer().emitConversion(value, from, to),
                                 fn->getArg(0));
  lowering_.builder_.CreateRetVoid();
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return fn;
}

}
