#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/result/from_throwable_lowerer/from_throwable_lowerer.h"
#include "compiler/library/result/result_combine_lowerer/result_combine_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The descriptor kind; must match runtime/core/tzrt/tzrt.h.
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

ResultLowerer::ResultLowerer(CodeGen &codegen) : codegen_(codegen) {}

CodeGen::ResultLayout ResultLowerer::resultLayout(const Type *type) {
  if (auto found = resultLayouts_.find(type); found != resultLayouts_.end()) {
    return found->second;
  }
  llvm::Type *value = lowerHalf(type->okType()), *error = lowerHalf(type->errType());
  const llvm::DataLayout &data = codegen_.module_->getDataLayout();
  bool errorFirst = data.getABITypeAlign(error).value() > data.getABITypeAlign(value).value();
  CodeGen::ResultLayout layout;
  layout.value = errorFirst ? 1 : 0;
  layout.error = errorFirst ? 0 : 1;
  layout.ok = 2;
  layout.type = llvm::StructType::get(
      codegen_.ctx_, {errorFirst ? error : value, errorFirst ? value : error, codegen_.builder_.getInt1Ty()});
  resultLayouts_[type] = layout;

  return layout;
}

llvm::Type *ResultLowerer::lowerHalf(const Type *half) {
  return half->isVoid() ? llvm::StructType::get(codegen_.ctx_) : codegen_.moduleBuilder().lower(half);
}

// { i32 kind, i32 size, ptr value type, ptr error type, i32 value offset,
// i32 error offset, i32 ok offset }: TzResultType in runtime/core/tzrt/tzrt.h.
llvm::Constant *ResultLowerer::resultDescriptor(const Type *type) {
  CodeGen::ResultLayout layout = resultLayout(type);
  const llvm::DataLayout &data = codegen_.module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout.type);
  llvm::Type *i32 = codegen_.builder_.getInt32Ty(), *ptr = codegen_.builder_.getPtrTy();
  auto *descriptorType = llvm::StructType::get(codegen_.ctx_, {i32, i32, ptr, ptr, i32, i32, i32});
  auto offset = [&](unsigned element) {
    return codegen_.builder_.getInt32(static_cast<uint32_t>(offsets->getElementOffset(element)));
  };
  llvm::Constant *init = llvm::ConstantStruct::get(
      descriptorType, {codegen_.builder_.getInt32(kResultKind),
                       codegen_.builder_.getInt32(static_cast<uint32_t>(data.getTypeAllocSize(layout.type))),
                       codegen_.typeDescriptors().typeDescriptor(type->okType()), codegen_.typeDescriptors().typeDescriptor(type->errType()),
                       offset(layout.value), offset(layout.error), offset(layout.ok)});
  auto *global =
      new llvm::GlobalVariable(*codegen_.module_, descriptorType, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.type." + type->name());
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  codegen_.typeDescriptors_[type] = global;

  return global;
}

llvm::Value *ResultLowerer::resultIsOk(llvm::Value *result, const Type *type) {
  return codegen_.builder_.CreateExtractValue(result, resultLayout(type).ok, "ok");
}

llvm::Value *ResultLowerer::resultHalf(llvm::Value *result, const Type *type, bool error) {
  CodeGen::ResultLayout layout = resultLayout(type);

  return codegen_.builder_.CreateExtractValue(result, error ? layout.error : layout.value,
                                     error ? "error" : "value");
}

llvm::Value *ResultLowerer::makeResult(const Type *type, llvm::Value *ok, llvm::Value *value,
                                 llvm::Value *error) {
  CodeGen::ResultLayout layout = resultLayout(type);
  llvm::Value *elements[3];
  elements[layout.value] =
      value ? value : llvm::Constant::getNullValue(layout.type->getElementType(layout.value));
  elements[layout.error] =
      error ? error : llvm::Constant::getNullValue(layout.type->getElementType(layout.error));
  elements[layout.ok] = ok;
  // `ok(1)` is a constant, which needs no counting.
  if (llvm::all_of(elements, [](llvm::Value *v) { return isa<llvm::Constant>(v); })) {
    return llvm::ConstantStruct::get(layout.type, {cast<llvm::Constant>(elements[0]),
                                                   cast<llvm::Constant>(elements[1]),
                                                   cast<llvm::Constant>(elements[2])});
  }
  llvm::Value *result = llvm::UndefValue::get(layout.type);
  for (unsigned i = 0; i < 3; ++i) {
    result = codegen_.builder_.CreateInsertValue(result, elements[i], i);
  }

  return result;
}

llvm::Value *ResultLowerer::convertHalf(llvm::Value *half, const Type *from, const Type *to) {
  if (from == to) {
    return half;
  }
  if (from->isNever() || to->isNever() || to->isVoid()) {
    return nullptr; // nothing to keep: zero
  }

  return codegen_.exprLowerer().emitConversion(half, from, to);
}

llvm::Value *ResultLowerer::callWithHalf(llvm::Value *fn, const Type *fnType, llvm::Value *half,
                                   const Type *halfType) {
  std::vector<llvm::Value *> args;
  if (fnType->params().empty()) {
    codegen_.refCounts().releaseValue(half, halfType); // the function does not take it
  } else {
    // The function owns its argument; `ok()` holds undefined.
    const Type *param = fnType->params()[0];
    const Type *given = halfType->isVoid() ? Type::undefinedTy() : halfType;
    llvm::Value *arg = halfType->isVoid() ? codegen_.builder_.getFalse() : half;
    args.push_back(given == param ? arg : codegen_.exprLowerer().emitConversion(arg, given, param));
  }
  llvm::Value *result = codegen_.closureLowerer().callFunctionValue(fn, fnType, args);
  codegen_.emitThrowCheckIf(codegen_.typeThrows(fnType));

  return result;
}

llvm::Value *ResultLowerer::emitResultCall(CallExpr *call) {
  switch (call->builtin) {
  case Builtin::ResultOk:
  case Builtin::ResultErr: {
    bool isOk = call->builtin == Builtin::ResultOk;
    const Type *type = call->type;
    llvm::Value *half = call->args.empty() ? nullptr : codegen_.exprLowerer().emitExpr(call->args[0]);
    // `ok(undefined)` holds nothing, as `ok()` does.
    if ((isOk ? type->okType() : type->errType())->isVoid()) {
      half = nullptr;
    }
    return makeResult(type, codegen_.builder_.getInt1(isOk), isOk ? half : nullptr, isOk ? nullptr : half);
  }
  case Builtin::SafeTry: {
    // The generator runs once, here, as an ordinary function whose `yield*`
    // returns an Err (see emitSafeTryYield).
    const Type *fnType = call->args[0]->type;
    llvm::Value *fn = codegen_.exprLowerer().emitExpr(call->args[0]);
    llvm::Value *result = codegen_.closureLowerer().callFunctionValue(fn, fnType, {});
    codegen_.refCounts().releaseValue(fn, fnType);
    codegen_.emitThrowCheckIf(codegen_.typeThrows(fnType));
    return result;
  }
  case Builtin::ResultCombine:
  case Builtin::ResultCombineWithAllErrors:
    return codegen_.resultCombineLowerer().emitCombine(call);
  case Builtin::ResultFromThrowable:
    return codegen_.fromThrowableLowerer().emitFromThrowable(call);
  case Builtin::ResultUnsafeUnwrap:
  case Builtin::ResultUnsafeUnwrapErr:
    return codegen_.fromThrowableLowerer().emitUnsafeUnwrap(call);
  default:
    return emitResultMethod(call);
  }
}

llvm::Value *ResultLowerer::emitResultMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  const Type *valueType = type->okType(), *errorType = type->errType();
  const Type *resultType = call->type;
  Builtin method = call->builtin;
  if (method == Builtin::ResultIsOk || method == Builtin::ResultIsErr) {
    CodeGen::Operand result = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    llvm::Value *ok = resultIsOk(result.value, type);
    codegen_.refCounts().dropOperand(result);
    return method == Builtin::ResultIsOk ? ok : codegen_.builder_.CreateNot(ok);
  }

  // The Result is taken over: the code for each half uses the half that is
  // there, and the other one is zero. The arguments are evaluated before
  // either runs, as in JavaScript, except a function for a half the Result
  // cannot have, which is never called.
  llvm::Value *result = codegen_.exprLowerer().emitExpr(member->object);
  std::vector<llvm::Value *> args(call->args.size(), nullptr);
  for (size_t i = 0; i < call->args.size(); ++i) {
    const Type *half = argGetsError(method, i) ? errorType : valueType;
    if (!(half->isNever() && isa<FunctionExpr>(call->args[i]) &&
          method != Builtin::ResultUnwrapOr)) {
      args[i] = codegen_.exprLowerer().emitExpr(call->args[i]);
    }
  }
  auto fnType = [&](size_t i) { return call->args[i]->type; };
  // The functions wait in slots while one of them may throw.
  bool throws = method != Builtin::ResultUnwrapOr &&
                llvm::any_of(call->args, [&](Expr *arg) { return codegen_.typeThrows(arg->type); });
  std::vector<CodeGen::Held> held;
  for (size_t i = 0; i < args.size(); ++i) {
    held.push_back(codegen_.throwLowerer().hold(args[i], fnType(i), throws));
  }
  llvm::Value *value = resultHalf(result, type, /*error=*/false);
  llvm::Value *error = resultHalf(result, type, /*error=*/true);
  // A function whose result is dropped (andTee, orTee).
  auto drop = [&](llvm::Value *returned, size_t i) {
    if (returned && !fnType(i)->ret()->isVoid()) {
      codegen_.refCounts().releaseValue(returned, fnType(i)->ret());
    }
  };

  std::function<llvm::Value *()> onOk, onErr;
  switch (method) {
  case Builtin::ResultMap:
    onOk = [&] {
      llvm::Value *mapped = callWithHalf(args[0], fnType(0), value, valueType);

      return makeResult(resultType, codegen_.builder_.getTrue(),
                        resultType->okType()->isVoid() ? nullptr : mapped, nullptr);
    };
    onErr = [&] {
      return makeResult(resultType, codegen_.builder_.getFalse(), nullptr,
                        convertHalf(error, errorType, resultType->errType()));
    };
    break;
  case Builtin::ResultMapErr:
    onOk = [&] {
      return makeResult(resultType, codegen_.builder_.getTrue(),
                        convertHalf(value, valueType, resultType->okType()), nullptr);
    };
    onErr = [&] {
      llvm::Value *mapped = callWithHalf(args[0], fnType(0), error, errorType);

      return makeResult(resultType, codegen_.builder_.getFalse(), nullptr,
                        resultType->errType()->isVoid() ? nullptr : mapped);
    };
    break;
  case Builtin::ResultAndThen:
    onOk = [&] {
      return codegen_.exprLowerer().emitConversion(callWithHalf(args[0], fnType(0), value, valueType), fnType(0)->ret(),
                            resultType);
    };
    onErr = [&] {
      return makeResult(resultType, codegen_.builder_.getFalse(), nullptr,
                        convertHalf(error, errorType, resultType->errType()));
    };
    break;
  case Builtin::ResultOrElse:
    onOk = [&] {
      return makeResult(resultType, codegen_.builder_.getTrue(),
                        convertHalf(value, valueType, resultType->okType()), nullptr);
    };
    onErr = [&] {
      return codegen_.exprLowerer().emitConversion(callWithHalf(args[0], fnType(0), error, errorType), fnType(0)->ret(),
                            resultType);
    };
    break;
  case Builtin::ResultMatch:
    onOk = [&]() -> llvm::Value * {
      llvm::Value *given = callWithHalf(args[0], fnType(0), value, valueType);

      return resultType->isVoid() ? nullptr : codegen_.exprLowerer().emitConversion(given, fnType(0)->ret(), resultType);
    };
    onErr = [&]() -> llvm::Value * {
      llvm::Value *given = callWithHalf(args[1], fnType(1), error, errorType);

      return resultType->isVoid() ? nullptr : codegen_.exprLowerer().emitConversion(given, fnType(1)->ret(), resultType);
    };
    break;
  case Builtin::ResultUnwrapOr:
    onOk = [&] {
      codegen_.refCounts().releaseValue(args[0], resultType);

      return convertHalf(value, valueType, resultType);
    };
    onErr = [&] {
      codegen_.refCounts().releaseValue(error, errorType);

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
      codegen_.refCounts().retainValue(half, halfType);
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
      codegen_.refCounts().retainValue(value, valueType);
      const Type *passed = fnType(0)->ret();
      llvm::Value *through = callWithHalf(args[0], fnType(0), value, valueType);
      auto pass = [&] {
        codegen_.refCounts().releaseValue(through, passed);

        return codegen_.exprLowerer().emitConversion(result, type, resultType);
      };
      auto fail = [&] {
        codegen_.refCounts().releaseValue(result, type);

        return makeResult(resultType, codegen_.builder_.getFalse(), nullptr,
                          convertHalf(resultHalf(through, passed, true), passed->errType(),
                                      resultType->errType()));
      };
      if (passed->errType()->isNever()) {
        return pass();
      }
      if (passed->okType()->isNever()) {
        return fail();
      }
      llvm::BasicBlock *passBlock = codegen_.functionFrame().newBlock("through.ok");
      llvm::BasicBlock *failBlock = codegen_.functionFrame().newBlock("through.err");
      llvm::BasicBlock *doneBlock = codegen_.functionFrame().newBlock("through.end");
      codegen_.builder_.CreateCondBr(resultIsOk(through, passed), passBlock, failBlock);
      codegen_.functionFrame().switchTo(passBlock);
      llvm::Value *a = pass();
      llvm::BasicBlock *passEnd = codegen_.builder_.GetInsertBlock();
      codegen_.builder_.CreateBr(doneBlock);
      codegen_.functionFrame().switchTo(failBlock);
      llvm::Value *b = fail();
      llvm::BasicBlock *failEnd = codegen_.builder_.GetInsertBlock();
      codegen_.builder_.CreateBr(doneBlock);
      codegen_.functionFrame().switchTo(doneBlock);
      llvm::PHINode *phi = codegen_.builder_.CreatePHI(a->getType(), 2);
      phi->addIncoming(a, passEnd);
      phi->addIncoming(b, failEnd);

      return phi;
    };
    onErr = [&] { return codegen_.exprLowerer().emitConversion(result, type, resultType); };
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
    llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("result.ok");
    llvm::BasicBlock *errBlock = codegen_.functionFrame().newBlock("result.err");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("result.end");
    codegen_.builder_.CreateCondBr(resultIsOk(result, type), okBlock, errBlock);
    codegen_.functionFrame().switchTo(okBlock);
    llvm::Value *a = onOk();
    llvm::BasicBlock *okEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(errBlock);
    llvm::Value *b = onErr();
    llvm::BasicBlock *errEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    returned = nullptr;
    if (a && b) {
      llvm::PHINode *phi = codegen_.builder_.CreatePHI(a->getType(), 2);
      phi->addIncoming(a, okEnd);
      phi->addIncoming(b, errEnd);
      returned = phi;
    }
  }
  for (CodeGen::Held &h : held) {
    codegen_.throwLowerer().take(h);
  }
  if (method != Builtin::ResultUnwrapOr) {
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i]) {
        codegen_.refCounts().releaseValue(args[i], fnType(i));
      }
    }
  }

  return returned;
}

llvm::Value *ResultLowerer::emitResultMember(MemberExpr *member) {
  // Sema allows only the half the type says is there, so the other one is
  // zero.
  const Type *type = member->object->type;
  bool error = member->name == "error";
  const Type *half = error ? type->errType() : type->okType();
  CodeGen::Operand result = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
  if (half->isVoid()) { // `ok().value` is undefined
    codegen_.refCounts().dropOperand(result);
    return codegen_.builder_.getFalse();
  }
  llvm::Value *value = resultHalf(result.value, type, error);
  if (!result.owned) {
    codegen_.refCounts().retainValue(value, half);
  }

  return value;
}

// `yield* r`: an Err returns from the generator of safeTry, as `return r`
// would, with its error converted to what the generator returns; an Ok gives
// its value. Sema allows it only where nothing computed before it in its
// statement is still held, so the variables are all there is to release.
llvm::Value *ResultLowerer::emitSafeTryYield(YieldExpr *yield) {
  const Type *type = yield->value->type;
  llvm::Value *result = codegen_.exprLowerer().emitExpr(yield->value);
  if (!type->errType()->isNever()) {
    llvm::BasicBlock *errBlock = codegen_.functionFrame().newBlock("yield.err");
    llvm::BasicBlock *okBlock = codegen_.functionFrame().newBlock("yield.ok");
    codegen_.builder_.CreateCondBr(resultIsOk(result, type), okBlock, errBlock,
                          llvm::MDBuilder(codegen_.ctx_).createLikelyBranchWeights());
    codegen_.functionFrame().switchTo(errBlock);
    const Type *err = Type::result(Type::never(), type->errType());
    llvm::Value *returned = codegen_.exprLowerer().emitConversion(
        makeResult(err, codegen_.builder_.getFalse(), nullptr, resultHalf(result, type, /*error=*/true)),
        err, codegen_.returnType_);
    codegen_.functionFrame().emitCleanups(0);
    codegen_.builder_.CreateRet(returned);
    codegen_.functionFrame().switchTo(okBlock);
  }
  if (type->okType()->isNever()) { // always an Err: the code after it never runs
    codegen_.builder_.CreateUnreachable();
    codegen_.functionFrame().startDeadBlock();
    return llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(yield->type));
  }
  if (type->okType()->isVoid()) {
    return codegen_.builder_.getFalse(); // undefined
  }

  return resultHalf(result, type, /*error=*/false);
}

} // namespace tz
