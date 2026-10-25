#include "compiler/lower/async/promise_combine_lowerer/promise_combine_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

enum CombineKind : int32_t { kAll, kAllSettled, kRace, kAny };

} // namespace

PromiseCombineLowerer::PromiseCombineLowerer(CodeGen &codegen) : codegen_(codegen) {}

// Promise.all, allSettled, race and any: the runtime gets an array of the
// promises, a value that is none becoming a resolved one, as in JavaScript.
llvm::Value *PromiseCombineLowerer::emitPromiseCombine(CallExpr *call) {
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty(), *i64 = codegen_.builder_.getInt64Ty();
  const Type *anyPromise = Type::promise(Type::voidTy());
  Expr *arg = call->args[0];
  const Type *argType = arg->type;
  auto toPromise = [&](llvm::Value *value, const Type *type) -> llvm::Value * {
    if (type->isPromise()) {
      return value;
    }
    auto *fn = cast<llvm::Function>(codegen_.asyncLowerer().asyncFunction("tz_promise_resolved", ptr, {ptr}).getCallee());
    llvm::Value *promise = codegen_.builder_.CreateCall(fn, {codegen_.promiseLowerer().promiseLayout(type)}, "promise");
    if (codegen_.promiseLowerer().promiseValueSize(type)) {
      codegen_.builder_.CreateStore(value, codegen_.promiseLowerer().promiseValueAddress(promise));
    }

    return promise;
  };
  // The values, each made a promise, in a new array.
  auto arrayOf = [&](std::vector<llvm::Value *> values, llvm::ArrayRef<const Type *> types) {
    llvm::Value *array =
        codegen_.builder_.CreateCall(codegen_.rt_.arrNew,
                            {codegen_.typeDescriptors().typeDescriptor(anyPromise), codegen_.builder_.getInt64(values.size()),
                             codegen_.builder_.getInt64(values.size())},
                            "promises");
    llvm::Value *data = codegen_.arrayLowerer().arrayData(array);
    for (size_t i = 0; i < values.size(); ++i) {
      codegen_.builder_.CreateStore(toPromise(values[i], types[i]),
                           codegen_.builder_.CreateConstInBoundsGEP1_64(ptr, data, i));
    }

    return array;
  };
  llvm::Value *promises;
  // What each promise of a tuple gives, which may differ.
  std::vector<const Type *> given;
  // A literal of the values themselves; `[...xs]` makes an array first.
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
    promises = arrayOf(codegen_.throwLowerer().emitHeldValues(exprs), types);
  } else if (argType->isTuple()) {
    llvm::Value *tuple = codegen_.exprLowerer().emitExpr(arg);
    std::vector<llvm::Value *> values;
    std::vector<const Type *> types;
    for (const Field &f : argType->fields()) {
      given.push_back(f.type);
    }
    for (const Field &f : argType->fields()) {
      llvm::Value *v = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(f.type), codegen_.objectLayouts().fieldAddress(tuple, argType, f.name));
      codegen_.refCounts().retainValue(v, f.type);
      values.push_back(v);
      types.push_back(f.type);
    }
    codegen_.refCounts().releaseValue(tuple, argType);
    promises = arrayOf(values, types);
  } else if (argType->inner()->isPromise()) {
    promises = codegen_.exprLowerer().emitExpr(arg);
  } else {
    const Type *element = argType->inner();
    llvm::Value *values = codegen_.exprLowerer().emitExpr(arg);
    promises = codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_promise_resolve_all", ptr, {ptr, ptr, ptr}),
                                   {values, codegen_.promiseLowerer().promiseLayout(element), codegen_.typeDescriptors().typeDescriptor(anyPromise)},
                                   "promises");
    codegen_.refCounts().releaseValue(values, argType);
  }

  // How the runtime collects what it gives (TzCombine in
  // runtime/core/tzrt/tzrt.h).
  const Type *result = call->type->promised();
  int32_t kind = call->builtin == Builtin::PromiseAll          ? kAll
                 : call->builtin == Builtin::PromiseAllSettled ? kAllSettled
                 : call->builtin == Builtin::PromiseRace       ? kRace
                                                               : kAny;
  llvm::Constant *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  llvm::Constant *items = null, *tuple = null, *convert = null, *each = null;
  uint32_t tupleSize = 0;
  const llvm::DataLayout &dl = codegen_.module_->getDataLayout();
  auto *shapeType =
      llvm::StructType::get(codegen_.ctx_, {ptr, ptr, ptr, i32, i32, ptr, ptr, i32, i32, i32, ptr, ptr});
  // The layout, size, value type and field offsets of allSettled's result
  // object.
  struct Settled {
    llvm::Constant *layout, *valueType;
    uint32_t size = 0, status = 0, value = 0, reason = 0;
  };
  auto settledOf = [&](const Type *object) {
    const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(object);
    const llvm::StructLayout *offsets = dl.getStructLayout(layout.type);
    auto offset = [&](llvm::StringRef name) {
      return uint32_t(offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(object, name)]));
    };
    std::vector<unsigned> keys{ObjectLayouts::fieldIndex(object, "status")};
    if (object->field("value")) {
      keys.push_back(ObjectLayouts::fieldIndex(object, "value"));
    }
    keys.push_back(ObjectLayouts::fieldIndex(object, "reason"));
    Settled s{codegen_.objectLayouts().layoutDescriptor(object, keys), null};
    s.size = uint32_t(dl.getTypeAllocSize(layout.type));
    s.status = offset("status");
    s.reason = offset("reason");
    if (const Field *value = object->field("value")) {
      s.value = offset("value");
      s.valueType = codegen_.typeDescriptors().typeDescriptor(value->type);
    }

    return s;
  };
  auto shapeOf = [&](llvm::Constant *resultLayout, const Settled &s) {
    return llvm::ConstantStruct::get(
        shapeType, {resultLayout, items, tuple, codegen_.builder_.getInt32(tupleSize),
                    codegen_.builder_.getInt32(s.size), s.layout, s.valueType, codegen_.builder_.getInt32(s.status),
                    codegen_.builder_.getInt32(s.value), codegen_.builder_.getInt32(s.reason), convert, each});
  };
  Settled settled{null, null};
  if (result->isTuple()) {
    std::vector<unsigned> keys(result->fields().size());
    for (unsigned i = 0; i < keys.size(); ++i) {
      keys[i] = i;
    }
    tuple = codegen_.objectLayouts().layoutDescriptor(result, keys);
    tupleSize = uint32_t(dl.getTypeAllocSize(codegen_.objectLayouts().objectLayout(result).type));
  } else if (result->isArray()) {
    items = codegen_.typeDescriptors().typeDescriptor(result->inner());
  }
  if (kind == kAllSettled && result->isTuple()) {
    // A tuple of results, each of its own type.
    std::vector<llvm::Constant *> shapes;
    for (const Field &f : result->fields()) {
      shapes.push_back(shapeOf(null, settledOf(f.type)));
    }
    auto *eachType = llvm::ArrayType::get(shapeType, shapes.size());
    auto *global = new llvm::GlobalVariable(
        *codegen_.module_, eachType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantArray::get(eachType, shapes), "tz.promise.settled");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    each = global;
  } else if (kind == kAllSettled) {
    settled = settledOf(result->inner());
  }
  if ((kind == kRace || kind == kAny) && codegen_.promiseLowerer().promiseValueSize(result)) {
    // Promises of different values give one of a union.
    std::vector<llvm::Constant *> fns;
    bool converts = false;
    for (const Type *t : given) {
      const Type *value = t->isPromise() ? t->promised() : t;
      if (value == result || value->isNever() || !codegen_.promiseLowerer().promiseValueSize(value)) {
        fns.push_back(null);
        continue;
      }
      fns.push_back(combineConverter(value, result));
      converts = true;
    }
    if (converts) {
      auto *fnsType = llvm::ArrayType::get(ptr, fns.size());
      auto *global = new llvm::GlobalVariable(
          *codegen_.module_, fnsType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
          llvm::ConstantArray::get(fnsType, fns), "tz.promise.convert");
      global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      convert = global;
    }
  }
  auto *shape = new llvm::GlobalVariable(
      *codegen_.module_, shapeType, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      shapeOf(codegen_.promiseLowerer().promiseLayout(result), settled), "tz.promise.combine");
  shape->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  (void)i64;
  llvm::Value *promise =
      codegen_.builder_.CreateCall(codegen_.asyncLowerer().asyncFunction("tz_promise_combine", ptr, {i32, ptr, ptr}),
                          {codegen_.builder_.getInt32(uint32_t(kind)), promises, shape}, "promise");
  // The array may be a member of a group, as its promises may be.
  codegen_.builder_.CreateCall(codegen_.hasCycles_ ? codegen_.groupLowerer().groupAwareRelease(codegen_.arrReleaseFn_) : codegen_.arrReleaseFn_, {promises});

  return promise;
}

llvm::Function *PromiseCombineLowerer::combineConverter(const Type *from, const Type *to) {
  llvm::Function *&fn = combineConverters_[{from, to}];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  fn = llvm::Function::Create(llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr, ptr}, false),
                              llvm::GlobalValue::InternalLinkage, "tz.promise.convert",
                              codegen_.module_.get());
  codegen_.functionBuilder().setFunctionAttributes(fn);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  llvm::Function *outer = std::exchange(codegen_.function_, fn);
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
  llvm::Value *value = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(from), fn->getArg(1), "value");
  codegen_.refCounts().retainValue(value, from);
  codegen_.builder_.CreateStore(codegen_.exprLowerer().emitConversion(value, from, to), fn->getArg(0));
  codegen_.builder_.CreateRetVoid();
  codegen_.function_ = outer;
  codegen_.builder_.restoreIP(point);

  return fn;
}

} // namespace tz
