#include "compiler/lower/library/tz_modules/foreign_lowerer/foreign_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/cstring_lowerer/cstring_lowerer.h"
#include "compiler/program/library/ffi/foreign_out/foreign_out.h"
#include "llvm/IR/MDBuilder.h"
#include <string>
#include <vector>

namespace tz {

ForeignLowerer::ForeignLowerer(Lowering &owner)
    : lowering(owner) {}

void ForeignLowerer::emitForeign(FunctionDecl *fn) {
  llvm::Function *function = this->lowering.functions_.lookup(fn->sym);
  this->lowering.functionBuilder().beginFunction(function, fn);
  const FuncSymbol *sym = fn->sym;
  auto params = std::vector<llvm::Type *>();
  auto args = std::vector<llvm::Value *>();
  auto written = std::vector<llvm::Value *>();
  for (size_t index = 0; index < sym->params.size(); ++index) {
    llvm::Value *value = function->getArg(unsigned(index));
    const Type *type = sym->params[index];
    params.push_back(this->foreignType(type));
    args.push_back(this->foreignArgument(value, type, written));
  }
  auto *returned = this->foreignType(sym->getKnownReturnType());
  auto *type = llvm::FunctionType::get(returned, params, false);
  auto attributes = this->foreignAttributes(sym);
  auto callee = this->foreignFunction(fn, type, attributes);
  auto *result = this->lowering.builder.CreateCall(callee, args);
  result->setAttributes(attributes);
  for (llvm::Value *flag : written) {
    this->lowering.builder.CreateStore(this->lowering.builder.getTrue(), flag);
  }
  for (size_t index = 0; index < sym->params.size(); ++index) {
    this->lowering.refCounts().releaseValue(function->getArg(unsigned(index)), sym->params[index]);
  }
  this->returnForeign(fn, result);
  this->lowering.functionBuilder().finishFunction();
}

llvm::Type *ForeignLowerer::foreignType(const Type *type) {
  if (type->isVoid()) {
    return this->lowering.builder.getVoidTy();
  }
  if (type->isBool()) {
    return this->lowering.builder.getInt1Ty();
  }
  if (type->isInteger()) {
    return this->lowering.builder.getIntNTy(type->bitWidth());
  }
  if (type->isFloat()) {
    return this->lowering.builder.getDoubleTy();
  }

  return this->lowering.builder.getPtrTy();
}

llvm::Value *ForeignLowerer::foreignArgument(llvm::Value *value, const Type *type,
                                             std::vector<llvm::Value *> &written) {
  if (ForeignOut().isOut(type)) {
    return this->outSlot(value, type, written);
  }
  if (type->isCString()) {
    return this->lowering.cstringLowerer().textPointer(value);
  }
  if (type->nonOptional()->isCString()) {
    auto *isAbsent = this->lowering.builder.CreateIsNull(value);
    auto *text = this->lowering.cstringLowerer().textPointer(value);
    auto *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());

    return this->lowering.builder.CreateSelect(isAbsent, null, text);
  }
  if (type->isArray()) {
    return this->lowering.arrayLowerer().arrayData(value);
  }
  if (type->isBuffer() || type->isTypedArray()) {
    return this->lowering.bytesLowerer().bytesData(value);
  }

  return value;
}

llvm::Value *ForeignLowerer::outSlot(llvm::Value *value, const Type *type,
                                     std::vector<llvm::Value *> &written) {
  auto &layout = this->lowering.objectLayouts().objectLayout(type);
  const Type *fieldType = type->fields().front().type;
  auto *field = this->lowering.builder.CreateStructGEP(layout.type, value, layout.elements.front());
  if (UnionLayout::zeroIsUndefined(fieldType->nonOptional())) {
    return field;
  }
  auto *stored = this->lowering.moduleBuilder().lower(fieldType);
  written.push_back(this->lowering.builder.CreateStructGEP(stored, field, 1));

  return this->lowering.builder.CreateStructGEP(stored, field, 0);
}

llvm::FunctionCallee ForeignLowerer::foreignFunction(const FunctionDecl *fn,
                                                     llvm::FunctionType *type,
                                                     llvm::AttributeList attributes) {
  auto *found = this->lowering.module_->getFunction(fn->name);
  if (found != nullptr) {
    return llvm::FunctionCallee(type, found);
  }
  auto *declared = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, fn->name,
                                          this->lowering.module_.get());
  declared->setAttributes(attributes);

  return llvm::FunctionCallee(type, declared);
}

llvm::AttributeList ForeignLowerer::foreignAttributes(const FuncSymbol *sym) {
  auto &context = this->lowering.ctx;
  auto attributes = llvm::AttributeList().addFnAttribute(context, llvm::Attribute::NoUnwind);
  for (size_t index = 0; index < sym->params.size(); ++index) {
    auto extension = this->extensionOf(sym->params[index]);
    if (extension != llvm::Attribute::None) {
      attributes = attributes.addParamAttribute(context, unsigned(index), extension);
    }
  }
  auto extension = this->extensionOf(sym->getKnownReturnType());
  if (extension != llvm::Attribute::None) {
    attributes = attributes.addRetAttribute(context, extension);
  }

  return attributes;
}

llvm::Attribute::AttrKind ForeignLowerer::extensionOf(const Type *type) const {
  if (type->isBool()) {
    return llvm::Attribute::ZExt;
  }
  bool isNarrow = type->isInteger() && type->bitWidth() < 32;
  if (isNarrow && type->isUnsigned()) {
    return llvm::Attribute::ZExt;
  }
  if (isNarrow) {
    return llvm::Attribute::SExt;
  }

  return llvm::Attribute::None;
}

void ForeignLowerer::returnForeign(const FunctionDecl *fn, llvm::Value *result) {
  const Type *returnType = fn->sym->getKnownReturnType();
  if (returnType->isVoid()) {
    this->lowering.builder.CreateRetVoid();
    return;
  }
  if (returnType->isHandle() || returnType->isCString()) {
    this->checkNotNull(fn, result);
  }
  if (returnType->isCString()) {
    auto *copied = this->lowering.cstringLowerer().emitCopy(result);
    this->lowering.builder.CreateRet(copied);
    return;
  }
  if (returnType->nonOptional()->isCString()) {
    auto *copied = this->copyPresent(result);
    this->lowering.builder.CreateRet(copied);
    return;
  }
  this->lowering.builder.CreateRet(result);
}

llvm::Value *ForeignLowerer::copyPresent(llvm::Value *result) {
  auto *copyBlock = this->lowering.functionFrame().newBlock("foreign.copy");
  auto *endBlock = this->lowering.functionFrame().newBlock("foreign.copied");
  auto *entry = this->lowering.builder.GetInsertBlock();
  auto *isAbsent = this->lowering.builder.CreateIsNull(result);
  this->lowering.builder.CreateCondBr(isAbsent, endBlock, copyBlock);
  this->lowering.functionFrame().switchTo(copyBlock);
  auto *copied = this->lowering.cstringLowerer().emitCopy(result);
  auto *copiedBlock = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(endBlock);
  this->lowering.functionFrame().switchTo(endBlock);
  auto *joined = this->lowering.builder.CreatePHI(this->lowering.builder.getPtrTy(), 2);
  joined->addIncoming(llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy()), entry);
  joined->addIncoming(copied, copiedBlock);

  return joined;
}

void ForeignLowerer::checkNotNull(const FunctionDecl *fn, llvm::Value *result) {
  auto *nullBlock = this->lowering.functionFrame().newBlock("foreign.null");
  auto *valueBlock = this->lowering.functionFrame().newBlock("foreign.value");
  auto *isNull = this->lowering.builder.CreateIsNull(result);
  auto *weights = llvm::MDBuilder(this->lowering.ctx).createUnlikelyBranchWeights();
  this->lowering.builder.CreateCondBr(isNull, nullBlock, valueBlock, weights);
  this->lowering.functionFrame().switchTo(nullBlock);
  auto message = fn->name.str() + " returned NULL, its declaration says it never does";
  this->lowering.processLowerer().emitPanic(message);
  this->lowering.functionFrame().switchTo(valueBlock);
}

}
