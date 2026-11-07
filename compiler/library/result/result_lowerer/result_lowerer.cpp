#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/result/err_sites/err_sites.h"
#include "compiler/library/result/result_combine_lowerer/result_combine_lowerer.h"
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
using llvm::isa;

namespace tz {

namespace {

constexpr uint32_t kResultKind = 12;

}

bool ResultLowerer::argGetsError(Builtin method, size_t i) {
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

ResultLowerer::ResultLowerer(Lowering &owner)
    : lowering(owner) {}

Lowering::ResultLayout ResultLowerer::resultLayout(const Type *type) {
  if (auto found = this->resultLayouts.find(type); found != this->resultLayouts.end()) {
    return found->second;
  }
  llvm::Type *value = this->lowerHalf(type->okType());
  llvm::Type *error = this->lowerHalf(type->errType());
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  bool errorFirst = data.getABITypeAlign(error).value() > data.getABITypeAlign(value).value();
  Lowering::ResultLayout layout;
  layout.value = errorFirst ? 1 : 0;
  layout.error = errorFirst ? 0 : 1;
  layout.ok = 2;
  layout.site = 3;
  layout.type = llvm::StructType::get(
      this->lowering.ctx_, {errorFirst ? error : value, errorFirst ? value : error,
                            this->lowering.builder_.getInt1Ty(), this->lowering.errSites().type()});
  this->resultLayouts[type] = layout;

  return layout;
}

llvm::Type *ResultLowerer::lowerHalf(const Type *half) {
  return half->isVoid() ? llvm::StructType::get(this->lowering.ctx_)
                        : this->lowering.moduleBuilder().lower(half);
}

llvm::Constant *ResultLowerer::resultDescriptor(const Type *type) {
  Lowering::ResultLayout layout = this->resultLayout(type);
  const llvm::DataLayout &data = this->lowering.module_->getDataLayout();
  const llvm::StructLayout *offsets = data.getStructLayout(layout.type);
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  auto *descriptorType =
      llvm::StructType::get(this->lowering.ctx_, {i32, i32, ptr, ptr, i32, i32, i32, i32});
  auto offset = [&](unsigned element) {
    return this->lowering.builder_.getInt32(
        static_cast<uint32_t>(offsets->getElementOffset(element)));
  };
  llvm::Constant *init = llvm::ConstantStruct::get(
      descriptorType,
      {this->lowering.builder_.getInt32(kResultKind),
       this->lowering.builder_.getInt32(static_cast<uint32_t>(data.getTypeAllocSize(layout.type))),
       this->lowering.typeDescriptors().typeDescriptor(type->okType()),
       this->lowering.typeDescriptors().typeDescriptor(type->errType()), offset(layout.value),
       offset(layout.error), offset(layout.ok), offset(layout.site)});
  auto *global =
      new llvm::GlobalVariable(*this->lowering.module_, descriptorType, true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.type." + type->name());
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  this->lowering.typeDescriptors_[type] = global;

  return global;
}

llvm::Value *ResultLowerer::resultIsOk(llvm::Value *result, const Type *type) {
  return this->lowering.builder_.CreateExtractValue(result, this->resultLayout(type).ok, "ok");
}

llvm::Value *ResultLowerer::resultHalf(llvm::Value *result, const Type *type, bool error) {
  Lowering::ResultLayout layout = this->resultLayout(type);

  return this->lowering.builder_.CreateExtractValue(result, error ? layout.error : layout.value,
                                                    error ? "error" : "value");
}

llvm::Value *ResultLowerer::resultSite(llvm::Value *result, const Type *type) {
  return this->lowering.builder_.CreateExtractValue(result, this->resultLayout(type).site, "site");
}

llvm::Value *ResultLowerer::makeResult(const Type *type, llvm::Value *ok, llvm::Value *value,
                                       llvm::Value *error, llvm::Value *site) {
  Lowering::ResultLayout layout = this->resultLayout(type);
  llvm::Value *elements[4];
  elements[layout.value] =
      value ? value : llvm::Constant::getNullValue(layout.type->getElementType(layout.value));
  elements[layout.error] =
      error ? error : llvm::Constant::getNullValue(layout.type->getElementType(layout.error));
  elements[layout.site] = site ? site : this->lowering.errSites().none();
  elements[layout.ok] = ok;
  if (llvm::all_of(elements, [](llvm::Value *v) { return isa<llvm::Constant>(v); })) {
    return llvm::ConstantStruct::get(
        layout.type, {cast<llvm::Constant>(elements[0]), cast<llvm::Constant>(elements[1]),
                      cast<llvm::Constant>(elements[2]), cast<llvm::Constant>(elements[3])});
  }
  llvm::Value *result = llvm::UndefValue::get(layout.type);
  for (unsigned i = 0; i < 4; ++i) {
    result = this->lowering.builder_.CreateInsertValue(result, elements[i], i);
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

  return this->lowering.exprLowerer().emitConversion(half, from, to);
}

llvm::Value *ResultLowerer::callWithHalf(llvm::Value *fn, const Type *fnType, llvm::Value *half,
                                         const Type *halfType) {
  std::vector<llvm::Value *> args;
  if (fnType->params().empty()) {
    this->lowering.refCounts().releaseValue(half, halfType);
  } else {
    const Type *param = fnType->params()[0];
    const Type *given = halfType->isVoid() ? Type::undefinedTy() : halfType;
    llvm::Value *arg = halfType->isVoid() ? this->lowering.builder_.getFalse() : half;
    args.push_back(given == param ? arg
                                  : this->lowering.exprLowerer().emitConversion(arg, given, param));
  }
  llvm::Value *result = this->lowering.closureLowerer().callFunctionValue(fn, fnType, args);

  return result;
}

llvm::Value *ResultLowerer::catchFailure(llvm::Value *value, const Type *type, llvm::SMLoc loc) {
  return this->catchFailure(type, loc, [&] { return value; });
}

llvm::Value *ResultLowerer::catchFailure(const Type *type, llvm::SMLoc loc,
                                         llvm::function_ref<llvm::Value *()> okValue) {
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Value *slot = this->failedSlot();
  llvm::Value *failure = this->lowering.builder_.CreateLoad(ptr, slot, "failure");
  BasicBlock *failBlock = this->lowering.functionFrame().newBlock("call.failed");
  BasicBlock *okBlock = this->lowering.functionFrame().newBlock("call.ok");
  BasicBlock *endBlock = this->lowering.functionFrame().newBlock("call.end");
  this->lowering.builder_.CreateCondBr(
      this->lowering.builder_.CreateIsNotNull(failure), failBlock, okBlock,
      llvm::MDBuilder(this->lowering.ctx_).createUnlikelyBranchWeights());
  this->lowering.functionFrame().switchTo(failBlock);
  this->lowering.builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), slot);
  llvm::Value *failed = this->makeResult(type, this->lowering.builder_.getFalse(), nullptr, failure,
                                         this->lowering.errSites().site(loc));
  BasicBlock *failEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(okBlock);
  llvm::Value *value = okValue();
  llvm::Value *done = this->makeResult(type, this->lowering.builder_.getTrue(),
                                       type->okType()->isVoid() ? nullptr : value, nullptr);
  BasicBlock *okEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = this->lowering.builder_.CreatePHI(done->getType(), 2, "result");
  phi->addIncoming(failed, failEnd);
  phi->addIncoming(done, okEnd);

  return phi;
}

llvm::Value *ResultLowerer::failedSlot() {
  if (!this->thread) {
    auto *type = llvm::StructType::get(this->lowering.builder_.getPtrTy());
    this->thread =
        cast<llvm::GlobalVariable>(this->lowering.module_->getOrInsertGlobal("tz_thread", type));
    this->thread->setAlignment(llvm::Align(8));
    this->thread->setThreadLocal(this->lowering.parallel_);
  }

  return this->lowering.parallel_ ? this->lowering.builder_.CreateThreadLocalAddress(this->thread)
                                  : static_cast<llvm::Value *>(this->thread);
}

llvm::Value *ResultLowerer::isFailed() {
  return this->lowering.builder_.CreateIsNotNull(
      this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(), this->failedSlot()));
}

llvm::Function *ResultLowerer::failureMaker(const Type *errorType) {
  static const std::vector<MakerFields> orders = {{nullptr, {"code", "message"}}};

  return this->lowering.objectMaker().maker(errorType, orders);
}

llvm::Value *ResultLowerer::emitResultCall(CallExpr *call) {
  switch (call->builtin()) {
    case Builtin::ResultOk:
    case Builtin::ResultErr: {
      bool isOk = call->builtin() == Builtin::ResultOk;
      const Type *type = call->type;
      llvm::Value *half =
          call->args.empty() ? nullptr : this->lowering.exprLowerer().emitExpr(call->args[0]);
      if ((isOk ? type->okType() : type->errType())->isVoid()) {
        half = nullptr;
      }
      return this->makeResult(type, this->lowering.builder_.getInt1(isOk), isOk ? half : nullptr,
                              isOk ? nullptr : half,
                              isOk ? nullptr : this->lowering.errSites().site(call->loc()));
    }
    case Builtin::ResultCombine:
    case Builtin::ResultCombineWithAllErrors:
      return this->lowering.resultCombineLowerer().emitCombine(call);
    default:
      return this->emitResultMethod(call);
  }
}

llvm::Value *ResultLowerer::emitResultMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  const Type *valueType = type->okType();
  const Type *errorType = type->errType();
  const Type *resultType = call->type;
  Builtin method = call->builtin();
  if (method == Builtin::ResultIsOk || method == Builtin::ResultIsErr) {
    Lowering::Operand result = this->lowering.refCounts().emitOperand(member->object, true);
    llvm::Value *ok = this->resultIsOk(result.value, type);
    this->lowering.refCounts().dropOperand(result);
    return method == Builtin::ResultIsOk ? ok : this->lowering.builder_.CreateNot(ok);
  }

  llvm::Value *result = this->lowering.exprLowerer().emitExpr(member->object);
  std::vector<llvm::Value *> args(call->args.size(), nullptr);
  for (size_t i = 0; i < call->args.size(); ++i) {
    const Type *half = ResultLowerer::argGetsError(method, i) ? errorType : valueType;
    if (!(half->isNever() && isa<FunctionExpr>(call->args[i]) &&
          method != Builtin::ResultUnwrapOr)) {
      args[i] = this->lowering.exprLowerer().emitExpr(call->args[i]);
    }
  }
  auto fnType = [&](size_t i) { return call->args[i]->type; };
  llvm::Value *value = this->resultHalf(result, type, false);
  llvm::Value *error = this->resultHalf(result, type, true);
  llvm::Value *site = this->resultSite(result, type);
  auto drop = [&](llvm::Value *returned, size_t i) {
    if (returned && !fnType(i)->ret()->isVoid()) {
      this->lowering.refCounts().releaseValue(returned, fnType(i)->ret());
    }
  };

  std::function<llvm::Value *()> onOk, onErr;
  switch (method) {
  case Builtin::ResultMap:
    onOk = [&] {
      llvm::Value *mapped = this->callWithHalf(args[0], fnType(0), value, valueType);

      return this->makeResult(resultType, this->lowering.builder_.getTrue(),
                              resultType->okType()->isVoid() ? nullptr : mapped, nullptr);
    };
    onErr = [&] {
      return this->makeResult(resultType, this->lowering.builder_.getFalse(), nullptr,
                              this->convertHalf(error, errorType, resultType->errType()), site);
    };
    break;
  case Builtin::ResultMapErr:
    onOk = [&] {
      return this->makeResult(resultType, this->lowering.builder_.getTrue(),
                              this->convertHalf(value, valueType, resultType->okType()), nullptr);
    };
    onErr = [&] {
      llvm::Value *mapped = this->callWithHalf(args[0], fnType(0), error, errorType);

      return this->makeResult(resultType, this->lowering.builder_.getFalse(), nullptr,
                              resultType->errType()->isVoid() ? nullptr : mapped, site);
    };
    break;
  case Builtin::ResultAndThen:
    onOk = [&] {
      return this->lowering.exprLowerer().emitConversion(
          this->callWithHalf(args[0], fnType(0), value, valueType), fnType(0)->ret(), resultType);
    };
    onErr = [&] {
      return this->makeResult(resultType, this->lowering.builder_.getFalse(), nullptr,
                              this->convertHalf(error, errorType, resultType->errType()), site);
    };
    break;
  case Builtin::ResultOrElse:
    onOk = [&] {
      return this->makeResult(resultType, this->lowering.builder_.getTrue(),
                              this->convertHalf(value, valueType, resultType->okType()), nullptr);
    };
    onErr = [&] {
      return this->lowering.exprLowerer().emitConversion(
          this->callWithHalf(args[0], fnType(0), error, errorType), fnType(0)->ret(), resultType);
    };
    break;
  case Builtin::ResultMatch:
    onOk = [&]() -> llvm::Value * {
      llvm::Value *given = this->callWithHalf(args[0], fnType(0), value, valueType);

      return resultType->isVoid()
                 ? nullptr
                 : this->lowering.exprLowerer().emitConversion(given, fnType(0)->ret(), resultType);
    };
    onErr = [&]() -> llvm::Value * {
      llvm::Value *given = this->callWithHalf(args[1], fnType(1), error, errorType);

      return resultType->isVoid()
                 ? nullptr
                 : this->lowering.exprLowerer().emitConversion(given, fnType(1)->ret(), resultType);
    };
    break;
  case Builtin::ResultUnwrapOr:
    onOk = [&] {
      this->lowering.refCounts().releaseValue(args[0], resultType);

      return this->convertHalf(value, valueType, resultType);
    };
    onErr = [&] {
      this->lowering.refCounts().releaseValue(error, errorType);

      return args[0];
    };
    break;
  case Builtin::ResultAndTee:
  case Builtin::ResultOrTee: {
    bool onError = method == Builtin::ResultOrTee;
    auto tee = [&, onError] {
      llvm::Value *half = onError ? error : value;
      const Type *halfType = onError ? errorType : valueType;
      this->lowering.refCounts().retainValue(half, halfType);
      drop(this->callWithHalf(args[0], fnType(0), half, halfType), 0);

      return result;
    };
    auto keep = [&] { return result; };
    onOk = onError ? std::function<llvm::Value *()>(keep) : tee;
    onErr = onError ? std::function<llvm::Value *()>(tee) : keep;
    break;
  }
  case Builtin::ResultAndThrough:
    onOk = [&]() -> llvm::Value * {
      this->lowering.refCounts().retainValue(value, valueType);
      const Type *passed = fnType(0)->ret();
      llvm::Value *through = this->callWithHalf(args[0], fnType(0), value, valueType);
      auto pass = [&] {
        this->lowering.refCounts().releaseValue(through, passed);

        return this->lowering.exprLowerer().emitConversion(result, type, resultType);
      };
      auto fail = [&] {
        this->lowering.refCounts().releaseValue(result, type);

        return this->makeResult(resultType, this->lowering.builder_.getFalse(), nullptr,
                                this->convertHalf(this->resultHalf(through, passed, true),
                                                  passed->errType(), resultType->errType()),
                                this->resultSite(through, passed));
      };
      if (passed->errType()->isNever()) {
        return pass();
      }
      if (passed->okType()->isNever()) {
        return fail();
      }
      llvm::BasicBlock *passBlock = this->lowering.functionFrame().newBlock("through.ok");
      llvm::BasicBlock *failBlock = this->lowering.functionFrame().newBlock("through.err");
      llvm::BasicBlock *doneBlock = this->lowering.functionFrame().newBlock("through.end");
      this->lowering.builder_.CreateCondBr(this->resultIsOk(through, passed), passBlock, failBlock);
      this->lowering.functionFrame().switchTo(passBlock);
      llvm::Value *a = pass();
      llvm::BasicBlock *passEnd = this->lowering.builder_.GetInsertBlock();
      this->lowering.builder_.CreateBr(doneBlock);
      this->lowering.functionFrame().switchTo(failBlock);
      llvm::Value *b = fail();
      llvm::BasicBlock *failEnd = this->lowering.builder_.GetInsertBlock();
      this->lowering.builder_.CreateBr(doneBlock);
      this->lowering.functionFrame().switchTo(doneBlock);
      llvm::PHINode *phi = this->lowering.builder_.CreatePHI(a->getType(), 2);
      phi->addIncoming(a, passEnd);
      phi->addIncoming(b, failEnd);

      return phi;
    };
    onErr = [&] { return this->lowering.exprLowerer().emitConversion(result, type, resultType); };
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
    llvm::BasicBlock *okBlock = this->lowering.functionFrame().newBlock("result.ok");
    llvm::BasicBlock *errBlock = this->lowering.functionFrame().newBlock("result.err");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("result.end");
    this->lowering.builder_.CreateCondBr(this->resultIsOk(result, type), okBlock, errBlock);
    this->lowering.functionFrame().switchTo(okBlock);
    llvm::Value *a = onOk();
    llvm::BasicBlock *okEnd = this->lowering.builder_.GetInsertBlock();
    this->lowering.builder_.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(errBlock);
    llvm::Value *b = onErr();
    llvm::BasicBlock *errEnd = this->lowering.builder_.GetInsertBlock();
    this->lowering.builder_.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    returned = nullptr;
    if (a && b) {
      llvm::PHINode *phi = this->lowering.builder_.CreatePHI(a->getType(), 2);
      phi->addIncoming(a, okEnd);
      phi->addIncoming(b, errEnd);
      returned = phi;
    }
  }
  if (method != Builtin::ResultUnwrapOr) {
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i]) {
        this->lowering.refCounts().releaseValue(args[i], fnType(i));
      }
    }
  }

  return returned;
}

llvm::Value *ResultLowerer::emitResultMember(MemberExpr *member) {
  const Type *type = member->object->type;
  bool error = member->name == "error";
  const Type *half = error ? type->errType() : type->okType();
  Lowering::Operand result = this->lowering.refCounts().emitOperand(member->object, true);
  if (half->isVoid()) {
    this->lowering.refCounts().dropOperand(result);
    return this->lowering.builder_.getFalse();
  }
  llvm::Value *value = this->resultHalf(result.value, type, error);
  if (!result.owned) {
    this->lowering.refCounts().retainValue(value, half);
  }

  return value;
}

}
