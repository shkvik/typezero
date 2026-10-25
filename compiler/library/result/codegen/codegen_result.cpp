// Results of neverthrow: values of { T value, E error, i1 ok }. The halves lie
// by alignment, the larger first, so that no padding comes between them, and
// the flag last: `Result<int, string>` takes 24 bytes. The half not in use
// holds zero, as undefined does in `T | undefined`, so releasing a Result
// releases both halves without looking at the flag. A Result needs no memory
// of its own: `ok(1)` lives in registers, or in the slot of a variable.
//
// The methods test the flag and run the code for the half that is there; a
// half the type says cannot be there (`Result<int, never>`) has no code at
// all. A function given to a method is called directly, and an arrow written
// in the call is inlined by LLVM, so `parse(s).map(n => n * 2).match(...)`
// becomes plain branches.

#include "codegen/codegen.h"

#include "llvm/IR/MDBuilder.h"

#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The descriptor kind; must match runtime/tzrt.h.
constexpr uint32_t kResultKind = 12;

// The half of a Result that the argument `i` of a method gets.
bool argGetsError(Builtin method, size_t i) {
  switch (method) {
  case Builtin::ResultMapErr:
  case Builtin::ResultOrElse:
  case Builtin::ResultOrTee:
  case Builtin::ResultUnwrapOr: // the argument is the result for an Err
    return true;
  case Builtin::ResultMatch:
    return i == 1;
  default:
    return false;
  }
}

} // namespace

CodeGen::ResultLayout CodeGen::resultLayout(const Type *type) {
  if (auto found = resultLayouts_.find(type); found != resultLayouts_.end())
    return found->second;
  llvm::Type *value = lowerHalf(type->okType()), *error = lowerHalf(type->errType());
  const llvm::DataLayout &data = module_->getDataLayout();
  bool errorFirst = data.getABITypeAlign(error).value() > data.getABITypeAlign(value).value();
  ResultLayout layout;
  layout.value = errorFirst ? 1 : 0;
  layout.error = errorFirst ? 0 : 1;
  layout.ok = 2;
  layout.type = llvm::StructType::get(
      ctx_, {errorFirst ? error : value, errorFirst ? value : error, builder_.getInt1Ty()});
  resultLayouts_[type] = layout;
  return layout;
}

llvm::Type *CodeGen::lowerHalf(const Type *half) {
  return half->isVoid() ? llvm::StructType::get(ctx_) : lower(half);
}

// { i32 kind, i32 size, ptr value type, ptr error type, i32 value offset,
// i32 error offset, i32 ok offset }: TzResultType in runtime/tzrt.h.
llvm::Constant *CodeGen::resultDescriptor(const Type *type) {
  ResultLayout layout = resultLayout(type);
  const llvm::DataLayout &data = module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout.type);
  llvm::Type *i32 = builder_.getInt32Ty(), *ptr = builder_.getPtrTy();
  auto *descriptorType = llvm::StructType::get(ctx_, {i32, i32, ptr, ptr, i32, i32, i32});
  auto offset = [&](unsigned element) {
    return builder_.getInt32(static_cast<uint32_t>(offsets->getElementOffset(element)));
  };
  llvm::Constant *init = llvm::ConstantStruct::get(
      descriptorType, {builder_.getInt32(kResultKind),
                       builder_.getInt32(static_cast<uint32_t>(data.getTypeAllocSize(layout.type))),
                       typeDescriptor(type->okType()), typeDescriptor(type->errType()),
                       offset(layout.value), offset(layout.error), offset(layout.ok)});
  auto *global =
      new llvm::GlobalVariable(*module_, descriptorType, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.type." + type->name());
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  typeDescriptors_[type] = global;
  return global;
}

llvm::Value *CodeGen::resultIsOk(llvm::Value *result, const Type *type) {
  return builder_.CreateExtractValue(result, resultLayout(type).ok, "ok");
}

llvm::Value *CodeGen::resultHalf(llvm::Value *result, const Type *type, bool error) {
  ResultLayout layout = resultLayout(type);
  return builder_.CreateExtractValue(result, error ? layout.error : layout.value,
                                     error ? "error" : "value");
}

llvm::Value *CodeGen::makeResult(const Type *type, llvm::Value *ok, llvm::Value *value,
                                 llvm::Value *error) {
  ResultLayout layout = resultLayout(type);
  llvm::Value *elements[3];
  elements[layout.value] =
      value ? value : llvm::Constant::getNullValue(layout.type->getElementType(layout.value));
  elements[layout.error] =
      error ? error : llvm::Constant::getNullValue(layout.type->getElementType(layout.error));
  elements[layout.ok] = ok;
  // `ok(1)` is a constant, which needs no counting.
  if (llvm::all_of(elements, [](llvm::Value *v) { return isa<llvm::Constant>(v); }))
    return llvm::ConstantStruct::get(layout.type, {cast<llvm::Constant>(elements[0]),
                                                   cast<llvm::Constant>(elements[1]),
                                                   cast<llvm::Constant>(elements[2])});
  llvm::Value *result = llvm::UndefValue::get(layout.type);
  for (unsigned i = 0; i < 3; ++i)
    result = builder_.CreateInsertValue(result, elements[i], i);
  return result;
}

llvm::Value *CodeGen::convertHalf(llvm::Value *half, const Type *from, const Type *to) {
  if (from == to)
    return half;
  if (from->isNever() || to->isNever() || to->isVoid())
    return nullptr; // nothing to keep: zero
  return emitConversion(half, from, to);
}

llvm::Value *CodeGen::callWithHalf(llvm::Value *fn, const Type *fnType, llvm::Value *half,
                                   const Type *halfType) {
  std::vector<llvm::Value *> args;
  if (fnType->params().empty()) {
    releaseValue(half, halfType); // the function does not take it
  } else {
    // The function owns its argument; `ok()` holds undefined.
    const Type *param = fnType->params()[0];
    const Type *given = halfType->isVoid() ? Type::undefinedTy() : halfType;
    llvm::Value *arg = halfType->isVoid() ? builder_.getFalse() : half;
    args.push_back(given == param ? arg : emitConversion(arg, given, param));
  }
  llvm::Value *result = callFunctionValue(fn, fnType, args);
  emitThrowCheckIf(typeThrows(fnType));
  return result;
}

llvm::Value *CodeGen::emitResultCall(CallExpr *call) {
  switch (call->builtin) {
  case Builtin::ResultOk:
  case Builtin::ResultErr: {
    bool isOk = call->builtin == Builtin::ResultOk;
    const Type *type = call->type;
    llvm::Value *half = call->args.empty() ? nullptr : emitExpr(call->args[0]);
    // `ok(undefined)` holds nothing, as `ok()` does.
    if ((isOk ? type->okType() : type->errType())->isVoid())
      half = nullptr;
    return makeResult(type, builder_.getInt1(isOk), isOk ? half : nullptr, isOk ? nullptr : half);
  }
  case Builtin::SafeTry: {
    // The generator runs once, here, as an ordinary function whose `yield*`
    // returns an Err (see emitSafeTryYield).
    const Type *fnType = call->args[0]->type;
    llvm::Value *fn = emitExpr(call->args[0]);
    llvm::Value *result = callFunctionValue(fn, fnType, {});
    releaseValue(fn, fnType);
    emitThrowCheckIf(typeThrows(fnType));
    return result;
  }
  case Builtin::ResultCombine:
  case Builtin::ResultCombineWithAllErrors:
    return emitCombine(call);
  case Builtin::ResultFromThrowable:
    return emitFromThrowable(call);
  case Builtin::ResultUnsafeUnwrap:
  case Builtin::ResultUnsafeUnwrapErr:
    return emitUnsafeUnwrap(call);
  default:
    return emitResultMethod(call);
  }
}

llvm::Value *CodeGen::emitResultMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  const Type *valueType = type->okType(), *errorType = type->errType();
  const Type *resultType = call->type;
  Builtin method = call->builtin;
  if (method == Builtin::ResultIsOk || method == Builtin::ResultIsErr) {
    Operand result = emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *ok = resultIsOk(result.value, type);
    dropOperand(result);
    return method == Builtin::ResultIsOk ? ok : builder_.CreateNot(ok);
  }

  // The Result is taken over: the code for each half uses the half that is
  // there, and the other one is zero. The arguments are evaluated before
  // either runs, as in JavaScript, except a function for a half the Result
  // cannot have, which is never called.
  llvm::Value *result = emitExpr(member->object);
  std::vector<llvm::Value *> args(call->args.size(), nullptr);
  for (size_t i = 0; i < call->args.size(); ++i) {
    const Type *half = argGetsError(method, i) ? errorType : valueType;
    if (!(half->isNever() && isa<FunctionExpr>(call->args[i]) && method != Builtin::ResultUnwrapOr))
      args[i] = emitExpr(call->args[i]);
  }
  auto fnType = [&](size_t i) { return call->args[i]->type; };
  // The functions wait in slots while one of them may throw.
  bool throws = method != Builtin::ResultUnwrapOr &&
                llvm::any_of(call->args, [&](Expr *arg) { return typeThrows(arg->type); });
  std::vector<Held> held;
  for (size_t i = 0; i < args.size(); ++i)
    held.push_back(hold(args[i], fnType(i), throws));
  llvm::Value *value = resultHalf(result, type, /*error=*/false);
  llvm::Value *error = resultHalf(result, type, /*error=*/true);
  // A function whose result is dropped (andTee, orTee).
  auto drop = [&](llvm::Value *returned, size_t i) {
    if (returned && !fnType(i)->ret()->isVoid())
      releaseValue(returned, fnType(i)->ret());
  };

  std::function<llvm::Value *()> onOk, onErr;
  switch (method) {
  case Builtin::ResultMap:
    onOk = [&] {
      llvm::Value *mapped = callWithHalf(args[0], fnType(0), value, valueType);
      return makeResult(resultType, builder_.getTrue(),
                        resultType->okType()->isVoid() ? nullptr : mapped, nullptr);
    };
    onErr = [&] {
      return makeResult(resultType, builder_.getFalse(), nullptr,
                        convertHalf(error, errorType, resultType->errType()));
    };
    break;
  case Builtin::ResultMapErr:
    onOk = [&] {
      return makeResult(resultType, builder_.getTrue(),
                        convertHalf(value, valueType, resultType->okType()), nullptr);
    };
    onErr = [&] {
      llvm::Value *mapped = callWithHalf(args[0], fnType(0), error, errorType);
      return makeResult(resultType, builder_.getFalse(), nullptr,
                        resultType->errType()->isVoid() ? nullptr : mapped);
    };
    break;
  case Builtin::ResultAndThen:
    onOk = [&] {
      return emitConversion(callWithHalf(args[0], fnType(0), value, valueType), fnType(0)->ret(),
                            resultType);
    };
    onErr = [&] {
      return makeResult(resultType, builder_.getFalse(), nullptr,
                        convertHalf(error, errorType, resultType->errType()));
    };
    break;
  case Builtin::ResultOrElse:
    onOk = [&] {
      return makeResult(resultType, builder_.getTrue(),
                        convertHalf(value, valueType, resultType->okType()), nullptr);
    };
    onErr = [&] {
      return emitConversion(callWithHalf(args[0], fnType(0), error, errorType), fnType(0)->ret(),
                            resultType);
    };
    break;
  case Builtin::ResultMatch:
    onOk = [&]() -> llvm::Value * {
      llvm::Value *given = callWithHalf(args[0], fnType(0), value, valueType);
      return resultType->isVoid() ? nullptr : emitConversion(given, fnType(0)->ret(), resultType);
    };
    onErr = [&]() -> llvm::Value * {
      llvm::Value *given = callWithHalf(args[1], fnType(1), error, errorType);
      return resultType->isVoid() ? nullptr : emitConversion(given, fnType(1)->ret(), resultType);
    };
    break;
  case Builtin::ResultUnwrapOr:
    onOk = [&] {
      releaseValue(args[0], resultType);
      return convertHalf(value, valueType, resultType);
    };
    onErr = [&] {
      releaseValue(error, errorType);
      return args[0];
    };
    break;
  case Builtin::ResultAndTee:
  case Builtin::ResultOrTee: {
    // The function gets the half too, and the Result stays as it is.
    bool onError = method == Builtin::ResultOrTee;
    auto tee = [&, onError] {
      llvm::Value *half = onError ? error : value;
      const Type *halfType = onError ? errorType : valueType;
      retainValue(half, halfType);
      drop(callWithHalf(args[0], fnType(0), half, halfType), 0);
      return result;
    };
    auto keep = [&] { return result; };
    onOk = onError ? std::function<llvm::Value *()>(keep) : tee;
    onErr = onError ? std::function<llvm::Value *()>(tee) : keep;
    break;
  }
  case Builtin::ResultAndThrough:
    // The function gets the value; its Err replaces the Result, its Ok is
    // dropped.
    onOk = [&]() -> llvm::Value * {
      retainValue(value, valueType);
      const Type *passed = fnType(0)->ret();
      llvm::Value *through = callWithHalf(args[0], fnType(0), value, valueType);
      auto pass = [&] {
        releaseValue(through, passed);
        return emitConversion(result, type, resultType);
      };
      auto fail = [&] {
        releaseValue(result, type);
        return makeResult(resultType, builder_.getFalse(), nullptr,
                          convertHalf(resultHalf(through, passed, true), passed->errType(),
                                      resultType->errType()));
      };
      if (passed->errType()->isNever())
        return pass();
      if (passed->okType()->isNever())
        return fail();
      llvm::BasicBlock *passBlock = newBlock("through.ok");
      llvm::BasicBlock *failBlock = newBlock("through.err");
      llvm::BasicBlock *doneBlock = newBlock("through.end");
      builder_.CreateCondBr(resultIsOk(through, passed), passBlock, failBlock);
      switchTo(passBlock);
      llvm::Value *a = pass();
      llvm::BasicBlock *passEnd = builder_.GetInsertBlock();
      builder_.CreateBr(doneBlock);
      switchTo(failBlock);
      llvm::Value *b = fail();
      llvm::BasicBlock *failEnd = builder_.GetInsertBlock();
      builder_.CreateBr(doneBlock);
      switchTo(doneBlock);
      llvm::PHINode *phi = builder_.CreatePHI(a->getType(), 2);
      phi->addIncoming(a, passEnd);
      phi->addIncoming(b, failEnd);
      return phi;
    };
    onErr = [&] { return emitConversion(result, type, resultType); };
    break;
  default:
    llvm_unreachable("not a method of Result");
  }

  // The code for the half that can be there; both, after a test.
  llvm::Value *returned;
  if (valueType->isNever()) {
    returned = onErr();
  } else if (errorType->isNever()) {
    returned = onOk();
  } else {
    llvm::BasicBlock *okBlock = newBlock("result.ok");
    llvm::BasicBlock *errBlock = newBlock("result.err");
    llvm::BasicBlock *endBlock = newBlock("result.end");
    builder_.CreateCondBr(resultIsOk(result, type), okBlock, errBlock);
    switchTo(okBlock);
    llvm::Value *a = onOk();
    llvm::BasicBlock *okEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(errBlock);
    llvm::Value *b = onErr();
    llvm::BasicBlock *errEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBlock);
    switchTo(endBlock);
    returned = nullptr;
    if (a && b) {
      llvm::PHINode *phi = builder_.CreatePHI(a->getType(), 2);
      phi->addIncoming(a, okEnd);
      phi->addIncoming(b, errEnd);
      returned = phi;
    }
  }
  for (Held &h : held)
    take(h);
  if (method != Builtin::ResultUnwrapOr)
    for (size_t i = 0; i < args.size(); ++i)
      if (args[i])
        releaseValue(args[i], fnType(i));
  return returned;
}

// Result.combine: the values of the Results in an array, or the first Err;
// combineWithAllErrors: the values, or every error.
llvm::Value *CodeGen::emitCombine(CallExpr *call) {
  if (call->args[0]->type->isTuple())
    return emitCombineTuple(call);
  bool all = call->builtin == Builtin::ResultCombineWithAllErrors;
  const Type *arrayType = call->args[0]->type, *element = arrayType->inner();
  const Type *valueType = element->okType(), *errorType = element->errType();
  const Type *resultType = call->type;
  llvm::Type *i64 = builder_.getInt64Ty();

  Operand array = emitOperand(call->args[0], /*canBorrow=*/true);
  llvm::Value *length = arrayLength(array.value);
  llvm::Value *values = newArray(Type::array(valueType), builder_.getInt64(0), length, "values");
  bool collects = all && !errorType->isNever();
  llvm::Value *errors = collects ? newArray(Type::array(errorType), builder_.getInt64(0),
                                            builder_.getInt64(0), "errors")
                                 : nullptr;
  llvm::Value *resultSlot = createStackSlot(lower(resultType), "combined");

  llvm::Value *indexSlot = createStackSlot(i64, "index");
  builder_.CreateStore(builder_.getInt64(0), indexSlot);
  llvm::BasicBlock *headBlock = newBlock("combine");
  llvm::BasicBlock *bodyBlock = newBlock("combine.body");
  llvm::BasicBlock *okBlock = newBlock("combine.ok");
  llvm::BasicBlock *errBlock = newBlock("combine.err");
  llvm::BasicBlock *nextBlock = newBlock("combine.next");
  llvm::BasicBlock *endBlock = newBlock("combine.end");
  llvm::BasicBlock *doneBlock = newBlock("combine.done");
  emitBlock(headBlock);
  llvm::Value *i = builder_.CreateLoad(i64, indexSlot, "i");
  builder_.CreateCondBr(builder_.CreateICmpULT(i, length), bodyBlock, endBlock);

  switchTo(bodyBlock);
  llvm::Value *result = builder_.CreateLoad(
      lower(element), elementAddress(arrayData(array.value), element, i), "result");
  builder_.CreateCondBr(resultIsOk(result, element), okBlock, errBlock);

  // The array keeps its Results: what goes into the new arrays is counted
  // again.
  switchTo(okBlock);
  llvm::Value *value = resultHalf(result, element, /*error=*/false);
  retainValue(value, valueType);
  emitPush(values, {value}, valueType);
  builder_.CreateBr(nextBlock);

  switchTo(errBlock);
  llvm::Value *error = resultHalf(result, element, /*error=*/true);
  retainValue(error, errorType);
  if (all) {
    if (collects)
      emitPush(errors, {error}, errorType);
    builder_.CreateBr(nextBlock);
  } else {
    // The first Err is the result.
    releaseValue(values, Type::array(valueType));
    builder_.CreateStore(makeResult(resultType, builder_.getFalse(), nullptr, error), resultSlot);
    builder_.CreateBr(doneBlock);
  }

  switchTo(nextBlock);
  builder_.CreateStore(builder_.CreateAdd(i, builder_.getInt64(1)), indexSlot);
  builder_.CreateBr(headBlock);

  switchTo(endBlock);
  if (!collects) {
    builder_.CreateStore(makeResult(resultType, builder_.getTrue(), values, nullptr), resultSlot);
  } else {
    // Any error makes the result an Err with all of them.
    llvm::Value *failed = builder_.CreateICmpNE(arrayLength(errors), builder_.getInt64(0));
    llvm::Value *okResult = makeResult(resultType, builder_.getTrue(), values, nullptr);
    llvm::Value *errResult = makeResult(resultType, builder_.getFalse(), nullptr, errors);
    llvm::BasicBlock *failBlock = newBlock("combine.failed");
    llvm::BasicBlock *passBlock = newBlock("combine.passed");
    builder_.CreateCondBr(failed, failBlock, passBlock);
    switchTo(failBlock);
    releaseValue(values, Type::array(valueType));
    builder_.CreateStore(errResult, resultSlot);
    builder_.CreateBr(doneBlock);
    switchTo(passBlock);
    releaseValue(errors, resultType->errType());
    builder_.CreateStore(okResult, resultSlot);
  }
  emitBlock(doneBlock);
  dropOperand(array);
  return builder_.CreateLoad(lower(resultType), resultSlot);
}

// Result.combine([a, b]) of Results with values of different types: their
// values make a tuple. The Results are evaluated first, as the elements of
// the list are in JavaScript, and tested in order; no list is made.
llvm::Value *CodeGen::emitCombineTuple(CallExpr *call) {
  bool all = call->builtin == Builtin::ResultCombineWithAllErrors;
  auto *list = cast<ArrayLitExpr>(call->args[0]);
  const Type *resultType = call->type, *tupleType = resultType->okType();
  const Type *errorType = all && resultType->errType()->isArray() ? resultType->errType()->inner()
                                                                  : resultType->errType();
  std::vector<llvm::Value *> results;
  std::vector<const Type *> types;
  for (const ArrayLitExpr::Element &e : list->elements) {
    results.push_back(emitExpr(e.expr));
    types.push_back(e.expr->type);
  }
  llvm::Value *resultSlot = createStackSlot(lower(resultType), "combined");
  llvm::BasicBlock *doneBlock = newBlock("combine.done");
  llvm::Value *errors =
      all && !errorType->isNever()
          ? newArray(Type::array(errorType), builder_.getInt64(0), builder_.getInt64(0), "errors")
          : nullptr;

  for (size_t i = 0; i < results.size(); ++i) {
    if (types[i]->errType()->isNever())
      continue; // always an Ok
    llvm::BasicBlock *errBlock = newBlock("combine.err");
    llvm::BasicBlock *nextBlock = newBlock("combine.next");
    builder_.CreateCondBr(resultIsOk(results[i], types[i]), nextBlock, errBlock);
    switchTo(errBlock);
    llvm::Value *error = convertHalf(resultHalf(results[i], types[i], /*error=*/true),
                                     types[i]->errType(), errorType);
    if (errors) {
      emitPush(errors, {error}, errorType);
    } else {
      // The first Err is the result; the other Results are dropped.
      for (size_t j = 0; j < results.size(); ++j)
        if (j != i)
          releaseValue(results[j], types[j]);
      builder_.CreateStore(makeResult(resultType, builder_.getFalse(), nullptr, error), resultSlot);
      builder_.CreateBr(doneBlock);
      switchTo(nextBlock);
      continue;
    }
    emitBlock(nextBlock);
  }

  // The values go into a new tuple, unless some errors were collected.
  auto makeTuple = [&] {
    const ObjectLayout &layout = objectLayout(tupleType);
    std::vector<unsigned> keys;
    for (unsigned i = 0; i < results.size(); ++i)
      keys.push_back(i);
    llvm::Value *tuple = allocObject(tupleType, keys);
    for (unsigned i = 0; i < results.size(); ++i)
      builder_.CreateStore(resultHalf(results[i], types[i], /*error=*/false),
                           builder_.CreateStructGEP(layout.type, tuple, layout.elements[i]));
    adoptFields(tuple, tupleType);
    builder_.CreateStore(makeResult(resultType, builder_.getTrue(), tuple, nullptr), resultSlot);
  };
  if (!errors) {
    makeTuple();
  } else {
    llvm::BasicBlock *failBlock = newBlock("combine.failed");
    llvm::BasicBlock *passBlock = newBlock("combine.passed");
    builder_.CreateCondBr(builder_.CreateICmpNE(arrayLength(errors), builder_.getInt64(0)),
                          failBlock, passBlock);
    switchTo(failBlock);
    for (size_t i = 0; i < results.size(); ++i)
      releaseValue(resultHalf(results[i], types[i], /*error=*/false), types[i]->okType());
    builder_.CreateStore(makeResult(resultType, builder_.getFalse(), nullptr, errors), resultSlot);
    builder_.CreateBr(doneBlock);
    switchTo(passBlock);
    releaseValue(errors, resultType->errType());
    makeTuple();
  }
  emitBlock(doneBlock);
  return builder_.CreateLoad(lower(resultType), resultSlot);
}

llvm::Value *CodeGen::emitResultMember(MemberExpr *member) {
  // Sema allows only the half the type says is there, so the other one is
  // zero.
  const Type *type = member->object->type;
  bool error = member->name == "error";
  const Type *half = error ? type->errType() : type->okType();
  Operand result = emitOperand(member->object, /*canBorrow=*/true);
  if (half->isVoid()) { // `ok().value` is undefined
    dropOperand(result);
    return builder_.getFalse();
  }
  llvm::Value *value = resultHalf(result.value, type, error);
  if (!result.owned)
    retainValue(value, half);
  return value;
}

// `yield* r`: an Err returns from the generator of safeTry, as `return r`
// would, with its error converted to what the generator returns; an Ok gives
// its value. Sema allows it only where nothing computed before it in its
// statement is still held, so the variables are all there is to release.
llvm::Value *CodeGen::emitSafeTryYield(YieldExpr *yield) {
  const Type *type = yield->value->type;
  llvm::Value *result = emitExpr(yield->value);
  if (!type->errType()->isNever()) {
    llvm::BasicBlock *errBlock = newBlock("yield.err");
    llvm::BasicBlock *okBlock = newBlock("yield.ok");
    builder_.CreateCondBr(resultIsOk(result, type), okBlock, errBlock,
                          llvm::MDBuilder(ctx_).createLikelyBranchWeights());
    switchTo(errBlock);
    const Type *err = Type::result(Type::never(), type->errType());
    llvm::Value *returned = emitConversion(
        makeResult(err, builder_.getFalse(), nullptr, resultHalf(result, type, /*error=*/true)),
        err, returnType_);
    emitCleanups(0);
    builder_.CreateRet(returned);
    switchTo(okBlock);
  }
  if (type->okType()->isNever()) { // always an Err: the code after it never runs
    builder_.CreateUnreachable();
    startDeadBlock();
    return llvm::Constant::getNullValue(lower(yield->type));
  }
  if (type->okType()->isVoid())
    return builder_.getFalse(); // undefined
  return resultHalf(result, type, /*error=*/false);
}

} // namespace tz
