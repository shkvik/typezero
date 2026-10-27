#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/result/err_sites/err_sites.h"
#include "compiler/library/result/result_combine_lowerer/result_combine_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "llvm/IR/MDBuilder.h"
#include <functional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

constexpr uint32_t kResultKind = 12;

bool argGetsError(Builtin method, size_t i) {
  switch (method) {
  case Builtin::ResultMapErr:
  case Builtin::ResultOrElse:
  case Builtin::ResultOrTee:
  case Builtin::ResultUnwrapOr:
    return true;
  case Builtin::ResultMatch:
    return i == 1;
  default:
    return false;
  }
}

}

ResultLowerer::ResultLowerer(Lowering &lowering) : lowering_(lowering) {}

Lowering::ResultLayout ResultLowerer::resultLayout(const Type *type) {
  if (auto found = resultLayouts_.find(type); found != resultLayouts_.end()) {
    return found->second;
  }
  llvm::Type *value = lowerHalf(type->okType()), *error = lowerHalf(type->errType());
  const llvm::DataLayout &data = lowering_.module_->getDataLayout();
  bool errorFirst = data.getABITypeAlign(error).value() > data.getABITypeAlign(value).value();
  Lowering::ResultLayout layout;
  layout.value = errorFirst ? 1 : 0;
  layout.error = errorFirst ? 0 : 1;
  layout.ok = 2;
  layout.site = 3;
  layout.type = llvm::StructType::get(
      lowering_.ctx_, {errorFirst ? error : value, errorFirst ? value : error,
                       lowering_.builder_.getInt1Ty(), lowering_.errSites().type()});
  resultLayouts_[type] = layout;

  return layout;
}

llvm::Type *ResultLowerer::lowerHalf(const Type *half) {
  return half->isVoid() ? llvm::StructType::get(lowering_.ctx_)
                        : lowering_.moduleBuilder().lower(half);
}

llvm::Constant *ResultLowerer::resultDescriptor(const Type *type) {
  Lowering::ResultLayout layout = resultLayout(type);
  const llvm::DataLayout &data = lowering_.module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout.type);
  llvm::Type *i32 = lowering_.builder_.getInt32Ty(), *ptr = lowering_.builder_.getPtrTy();
  auto *descriptorType =
      llvm::StructType::get(lowering_.ctx_, {i32, i32, ptr, ptr, i32, i32, i32, i32});
  auto offset = [&](unsigned element) {
    return lowering_.builder_.getInt32(static_cast<uint32_t>(offsets->getElementOffset(element)));
  };
  llvm::Constant *init = llvm::ConstantStruct::get(
      descriptorType,
      {lowering_.builder_.getInt32(kResultKind),
       lowering_.builder_.getInt32(static_cast<uint32_t>(data.getTypeAllocSize(layout.type))),
       lowering_.typeDescriptors().typeDescriptor(type->okType()),
       lowering_.typeDescriptors().typeDescriptor(type->errType()), offset(layout.value),
       offset(layout.error), offset(layout.ok), offset(layout.site)});
  auto *global =
      new llvm::GlobalVariable(*lowering_.module_, descriptorType, true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.type." + type->name());
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  lowering_.typeDescriptors_[type] = global;

  return global;
}

llvm::Value *ResultLowerer::resultIsOk(llvm::Value *result, const Type *type) {
  return lowering_.builder_.CreateExtractValue(result, resultLayout(type).ok, "ok");
}

llvm::Value *ResultLowerer::resultHalf(llvm::Value *result, const Type *type, bool error) {
  Lowering::ResultLayout layout = resultLayout(type);

  return lowering_.builder_.CreateExtractValue(result, error ? layout.error : layout.value,
                                               error ? "error" : "value");
}

llvm::Value *ResultLowerer::resultSite(llvm::Value *result, const Type *type) {
  return lowering_.builder_.CreateExtractValue(result, resultLayout(type).site, "site");
}

llvm::Value *ResultLowerer::makeResult(const Type *type, llvm::Value *ok, llvm::Value *value,
                                       llvm::Value *error, llvm::Value *site) {
  Lowering::ResultLayout layout = resultLayout(type);
  llvm::Value *elements[4];
  elements[layout.value] =
      value ? value : llvm::Constant::getNullValue(layout.type->getElementType(layout.value));
  elements[layout.error] =
      error ? error : llvm::Constant::getNullValue(layout.type->getElementType(layout.error));
  elements[layout.site] = site ? site : lowering_.errSites().none();
  elements[layout.ok] = ok;
  if (llvm::all_of(elements, [](llvm::Value *v) { return isa<llvm::Constant>(v); })) {
    return llvm::ConstantStruct::get(
        layout.type, {cast<llvm::Constant>(elements[0]), cast<llvm::Constant>(elements[1]),
                      cast<llvm::Constant>(elements[2]), cast<llvm::Constant>(elements[3])});
  }
  llvm::Value *result = llvm::UndefValue::get(layout.type);
  for (unsigned i = 0; i < 4; ++i) {
    result = lowering_.builder_.CreateInsertValue(result, elements[i], i);
  }

  return result;
}

llvm::Value *ResultLowerer::convertHalf(llvm::Value *half, const Type *from, const Type *to) {
  if (from == to) {
    return half;
  }
  if (from->isNever() || to->isNever() || to->isVoid()) {
    return nullptr;
  }

  return lowering_.exprLowerer().emitConversion(half, from, to);
}

llvm::Value *ResultLowerer::callWithHalf(llvm::Value *fn, const Type *fnType, llvm::Value *half,
                                         const Type *halfType) {
  std::vector<llvm::Value *> args;
  if (fnType->params().empty()) {
    lowering_.refCounts().releaseValue(half, halfType);
  } else {
    const Type *param = fnType->params()[0];
    const Type *given = halfType->isVoid() ? Type::undefinedTy() : halfType;
    llvm::Value *arg = halfType->isVoid() ? lowering_.builder_.getFalse() : half;
    args.push_back(given == param ? arg
                                  : lowering_.exprLowerer().emitConversion(arg, given, param));
  }
  llvm::Value *result = lowering_.closureLowerer().callFunctionValue(fn, fnType, args);

  return result;
}

llvm::Value *ResultLowerer::catchFailure(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  return catchFailure(type, loc, [&] { return value; });
}

llvm::Value *ResultLowerer::catchFailure(const Type *type, llvm::SMLoc loc,
                                         llvm::function_ref<llvm::Value *()> okValue) {
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  llvm::Value *slot = lowering_.throwLowerer().failedSlot();
  llvm::Value *failure = lowering_.builder_.CreateLoad(ptr, slot, "failure");
  BasicBlock *failBlock = lowering_.functionFrame().newBlock("call.failed");
  BasicBlock *okBlock = lowering_.functionFrame().newBlock("call.ok");
  BasicBlock *endBlock = lowering_.functionFrame().newBlock("call.end");
  lowering_.builder_.CreateCondBr(lowering_.builder_.CreateIsNotNull(failure), failBlock, okBlock,
                                  llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(failBlock);
  lowering_.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), slot);
  llvm::Value *failed = makeResult(type, lowering_.builder_.getFalse(), nullptr, failure,
                                   lowering_.errSites().site(loc));
  BasicBlock *failEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(okBlock);
  llvm::Value *value = okValue();
  llvm::Value *done = makeResult(type, lowering_.builder_.getTrue(),
                                 type->okType()->isVoid() ? nullptr : value, nullptr);
  BasicBlock *okEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);
  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(done->getType(), 2, "result");
  phi->addIncoming(failed, failEnd);
  phi->addIncoming(done, okEnd);

  return phi;
}

llvm::Function *ResultLowerer::failureMaker(const Type *errorType) {
  static const std::vector<MakerFields> orders = {{nullptr, {"code", "message"}}};

  return lowering_.objectMaker().maker(errorType, orders);
}

llvm::Value *ResultLowerer::emitResultCall(CallExpr *call) {
  switch (call->builtin) {
  case Builtin::ResultOk:
  case Builtin::ResultErr: {
    bool isOk = call->builtin == Builtin::ResultOk;
    const Type *type = call->type;
    llvm::Value *half =
        call->args.empty() ? nullptr : lowering_.exprLowerer().emitExpr(call->args[0]);
    if ((isOk ? type->okType() : type->errType())->isVoid()) {
      half = nullptr;
    }
    return makeResult(type, lowering_.builder_.getInt1(isOk), isOk ? half : nullptr,
                      isOk ? nullptr : half,
                      isOk ? nullptr : lowering_.errSites().site(call->loc()));
  }
  case Builtin::ResultCombine:
  case Builtin::ResultCombineWithAllErrors:
    return lowering_.resultCombineLowerer().emitCombine(call);
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
    Lowering::Operand result = lowering_.refCounts().emitOperand(member->object, true);
    llvm::Value *ok = resultIsOk(result.value, type);
    lowering_.refCounts().dropOperand(result);
    return method == Builtin::ResultIsOk ? ok : lowering_.builder_.CreateNot(ok);
  }

  llvm::Value *result = lowering_.exprLowerer().emitExpr(member->object);
  std::vector<llvm::Value *> args(call->args.size(), nullptr);
  for (size_t i = 0; i < call->args.size(); ++i) {
    const Type *half = argGetsError(method, i) ? errorType : valueType;
    if (!(half->isNever() && isa<FunctionExpr>(call->args[i]) &&
          method != Builtin::ResultUnwrapOr)) {
      args[i] = lowering_.exprLowerer().emitExpr(call->args[i]);
    }
  }
  auto fnType = [&](size_t i) { return call->args[i]->type; };
  bool throws = method != Builtin::ResultUnwrapOr && llvm::any_of(call->args, [&](Expr *arg) {
                  return false;
                });
  std::vector<llvm::Value *> held;
  for (size_t i = 0; i < args.size(); ++i) {
    held.push_back(args[i]);
  }
  llvm::Value *value = resultHalf(result, type, false);
  llvm::Value *error = resultHalf(result, type, true);
  llvm::Value *site = resultSite(result, type);
  auto drop = [&](llvm::Value *returned, size_t i) {
    if (returned && !fnType(i)->ret()->isVoid()) {
      lowering_.refCounts().releaseValue(returned, fnType(i)->ret());
    }
  };

  std::function<llvm::Value *()> onOk, onErr;
  switch (method) {
  case Builtin::ResultMap:
    onOk = [&] {
      llvm::Value *mapped = callWithHalf(args[0], fnType(0), value, valueType);

      return makeResult(resultType, lowering_.builder_.getTrue(),
                        resultType->okType()->isVoid() ? nullptr : mapped, nullptr);
    };
    onErr = [&] {
      return makeResult(resultType, lowering_.builder_.getFalse(), nullptr,
                        convertHalf(error, errorType, resultType->errType()), site);
    };
    break;
  case Builtin::ResultMapErr:
    onOk = [&] {
      return makeResult(resultType, lowering_.builder_.getTrue(),
                        convertHalf(value, valueType, resultType->okType()), nullptr);
    };
    onErr = [&] {
      llvm::Value *mapped = callWithHalf(args[0], fnType(0), error, errorType);

      return makeResult(resultType, lowering_.builder_.getFalse(), nullptr,
                        resultType->errType()->isVoid() ? nullptr : mapped, site);
    };
    break;
  case Builtin::ResultAndThen:
    onOk = [&] {
      return lowering_.exprLowerer().emitConversion(
          callWithHalf(args[0], fnType(0), value, valueType), fnType(0)->ret(), resultType);
    };
    onErr = [&] {
      return makeResult(resultType, lowering_.builder_.getFalse(), nullptr,
                        convertHalf(error, errorType, resultType->errType()), site);
    };
    break;
  case Builtin::ResultOrElse:
    onOk = [&] {
      return makeResult(resultType, lowering_.builder_.getTrue(),
                        convertHalf(value, valueType, resultType->okType()), nullptr);
    };
    onErr = [&] {
      return lowering_.exprLowerer().emitConversion(
          callWithHalf(args[0], fnType(0), error, errorType), fnType(0)->ret(), resultType);
    };
    break;
  case Builtin::ResultMatch:
    onOk = [&]() -> llvm::Value * {
      llvm::Value *given = callWithHalf(args[0], fnType(0), value, valueType);

      return resultType->isVoid()
                 ? nullptr
                 : lowering_.exprLowerer().emitConversion(given, fnType(0)->ret(), resultType);
    };
    onErr = [&]() -> llvm::Value * {
      llvm::Value *given = callWithHalf(args[1], fnType(1), error, errorType);

      return resultType->isVoid()
                 ? nullptr
                 : lowering_.exprLowerer().emitConversion(given, fnType(1)->ret(), resultType);
    };
    break;
  case Builtin::ResultUnwrapOr:
    onOk = [&] {
      lowering_.refCounts().releaseValue(args[0], resultType);

      return convertHalf(value, valueType, resultType);
    };
    onErr = [&] {
      lowering_.refCounts().releaseValue(error, errorType);

      return args[0];
    };
    break;
  case Builtin::ResultAndTee:
  case Builtin::ResultOrTee: {
    bool onError = method == Builtin::ResultOrTee;
    auto tee = [&, onError] {
      llvm::Value *half = onError ? error : value;
      const Type *halfType = onError ? errorType : valueType;
      lowering_.refCounts().retainValue(half, halfType);
      drop(callWithHalf(args[0], fnType(0), half, halfType), 0);

      return result;
    };
    auto keep = [&] { return result; };
    onOk = onError ? std::function<llvm::Value *()>(keep) : tee;
    onErr = onError ? std::function<llvm::Value *()>(tee) : keep;
    break;
  }
  case Builtin::ResultAndThrough:
    onOk = [&]() -> llvm::Value * {
      lowering_.refCounts().retainValue(value, valueType);
      const Type *passed = fnType(0)->ret();
      llvm::Value *through = callWithHalf(args[0], fnType(0), value, valueType);
      auto pass = [&] {
        lowering_.refCounts().releaseValue(through, passed);

        return lowering_.exprLowerer().emitConversion(result, type, resultType);
      };
      auto fail = [&] {
        lowering_.refCounts().releaseValue(result, type);

        return makeResult(resultType, lowering_.builder_.getFalse(), nullptr,
                          convertHalf(resultHalf(through, passed, true), passed->errType(),
                                      resultType->errType()),
                          resultSite(through, passed));
      };
      if (passed->errType()->isNever()) {
        return pass();
      }
      if (passed->okType()->isNever()) {
        return fail();
      }
      llvm::BasicBlock *passBlock = lowering_.functionFrame().newBlock("through.ok");
      llvm::BasicBlock *failBlock = lowering_.functionFrame().newBlock("through.err");
      llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("through.end");
      lowering_.builder_.CreateCondBr(resultIsOk(through, passed), passBlock, failBlock);
      lowering_.functionFrame().switchTo(passBlock);
      llvm::Value *a = pass();
      llvm::BasicBlock *passEnd = lowering_.builder_.GetInsertBlock();
      lowering_.builder_.CreateBr(doneBlock);
      lowering_.functionFrame().switchTo(failBlock);
      llvm::Value *b = fail();
      llvm::BasicBlock *failEnd = lowering_.builder_.GetInsertBlock();
      lowering_.builder_.CreateBr(doneBlock);
      lowering_.functionFrame().switchTo(doneBlock);
      llvm::PHINode *phi = lowering_.builder_.CreatePHI(a->getType(), 2);
      phi->addIncoming(a, passEnd);
      phi->addIncoming(b, failEnd);

      return phi;
    };
    onErr = [&] { return lowering_.exprLowerer().emitConversion(result, type, resultType); };
    break;
  default:
    llvm_unreachable("not a method of Result");
  }

  llvm::Value *returned;
  if (valueType->isNever()) {
    returned = onErr();
  } else if (errorType->isNever()) {
    returned = onOk();
  } else {
    llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("result.ok");
    llvm::BasicBlock *errBlock = lowering_.functionFrame().newBlock("result.err");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("result.end");
    lowering_.builder_.CreateCondBr(resultIsOk(result, type), okBlock, errBlock);
    lowering_.functionFrame().switchTo(okBlock);
    llvm::Value *a = onOk();
    llvm::BasicBlock *okEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(errBlock);
    llvm::Value *b = onErr();
    llvm::BasicBlock *errEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    returned = nullptr;
    if (a && b) {
      llvm::PHINode *phi = lowering_.builder_.CreatePHI(a->getType(), 2);
      phi->addIncoming(a, okEnd);
      phi->addIncoming(b, errEnd);
      returned = phi;
    }
  }
  for (llvm::Value *&h : held) {
    h;
  }
  if (method != Builtin::ResultUnwrapOr) {
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i]) {
        lowering_.refCounts().releaseValue(args[i], fnType(i));
      }
    }
  }

  return returned;
}

llvm::Value *ResultLowerer::emitResultMember(MemberExpr *member) {
  const Type *type = member->object->type;
  bool error = member->name == "error";
  const Type *half = error ? type->errType() : type->okType();
  Lowering::Operand result = lowering_.refCounts().emitOperand(member->object, true);
  if (half->isVoid()) {
    lowering_.refCounts().dropOperand(result);
    return lowering_.builder_.getFalse();
  }
  llvm::Value *value = resultHalf(result.value, type, error);
  if (!result.owned) {
    lowering_.refCounts().retainValue(value, half);
  }

  return value;
}

}
