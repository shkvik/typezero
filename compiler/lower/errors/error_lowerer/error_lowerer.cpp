#include "compiler/lower/errors/error_lowerer/error_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

}

ErrorLowerer::ErrorLowerer(Lowering &lowering) : lowering_(lowering) {}

void ErrorLowerer::emitErrorConstructor(FunctionDecl *fn) {
  lowering_.functionBuilder().beginFunction(lowering_.functions_.lookup(fn->sym), fn);
  const Type *type = fn->cls->type;
  llvm::Value *self = lowering_.function_->getArg(0);
  llvm::Value *message = lowering_.function_->getArg(1);
  llvm::Value *options = lowering_.function_->getArg(2);
  const Type *messageType = fn->sym->params[0], *optionsType = fn->sym->params[1];
  llvm::Value *text = lowering_.builder_.CreateSelect(
      lowering_.optionalLowerer().optionalHas(message, messageType),
      lowering_.optionalLowerer().optionalValue(message, messageType),
      lowering_.stringLowerer().stringLiteral(u""));
  lowering_.builder_.CreateStore(text,
                                 lowering_.objectLayouts().fieldAddress(self, type, "message"));
  lowering_.builder_.CreateStore(lowering_.stringLowerer().stringLiteral(u"Error"),
                                 lowering_.objectLayouts().fieldAddress(self, type, "name"));
  llvm::BasicBlock *causeBlock = lowering_.functionFrame().newBlock("cause");
  llvm::BasicBlock *doneBlock = lowering_.functionFrame().newBlock("done");
  lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(options, optionsType),
                                  causeBlock, doneBlock);
  lowering_.functionFrame().switchTo(causeBlock);
  const Type *object = optionsType->inner();
  llvm::Value *cause = lowering_.builder_.CreateLoad(
      lowering_.builder_.getPtrTy(),
      lowering_.objectLayouts().fieldAddress(options, object, "cause"));
  lowering_.refCounts().retainValue(cause, Type::optional(type));
  lowering_.builder_.CreateStore(cause,
                                 lowering_.objectLayouts().fieldAddress(self, type, "cause"));
  lowering_.storeLowerer().groupStore(self, cause, Type::optional(type));
  if (lowering_.memory().hasCycles) {
    lowering_.builder_.CreateCall(lowering_.instanceReleaseFn_, {options});
  } else {
    lowering_.refCounts().releaseValue(options, object);
  }
  lowering_.builder_.CreateBr(doneBlock);
  lowering_.functionFrame().switchTo(doneBlock);
  lowering_.builder_.CreateRetVoid();
  lowering_.function_ = nullptr;
}

const ClassDecl *ErrorLowerer::errorKindClass(int kind) const {
  if (kind > 0) {
    for (const ClassDecl *child : lowering_.errorClass_->children) {
      if (child->builtin && child->name == kErrorKinds[kind]) {
        return child;
      }
    }
  }

  return lowering_.errorClass_;
}

llvm::Constant *ErrorLowerer::errorDescriptor(int kind) {
  return lowering_.classDescriptors().classDescriptor(errorKindClass(kind));
}

llvm::Function *ErrorLowerer::errorMaker() {
  if (errorMaker_) {
    return errorMaker_;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty(),
             *i64 = lowering_.builder_.getInt64Ty();
  errorMaker_ = llvm::Function::Create(llvm::FunctionType::get(ptr, {i32, ptr, i64}, false),
                                       llvm::GlobalValue::InternalLinkage, "tz.error.new",
                                       lowering_.module_.get());
  lowering_.functionBuilder().setFunctionAttributes(errorMaker_);
  errorMaker_->addFnAttr(llvm::Attribute::Cold);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, errorMaker_);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", errorMaker_));
  size_t count = std::size(kErrorKinds);
  std::vector<llvm::Constant *> descriptors;
  for (size_t k = 0; k < count; ++k) {
    descriptors.push_back(errorDescriptor(int(k)));
  }
  auto *tableType = llvm::ArrayType::get(ptr, count);
  auto *table = new llvm::GlobalVariable(
      *lowering_.module_, tableType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(tableType, descriptors), "tz.errors");
  llvm::Value *kind = errorMaker_->getArg(0);
  llvm::Value *descriptor =
      lowering_.builder_.CreateLoad(ptr,
                                    lowering_.builder_.CreateInBoundsGEP(
                                        tableType, table, {lowering_.builder_.getInt64(0), kind}),
                                    "layout");
  const Type *type = lowering_.errorClass_->type;
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object = lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                                      {lowering_.builder_.getInt64(size)}, "error");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                 lowering_.builder_.CreateStructGEP(layout.type, object, 0));
  lowering_.builder_.CreateStore(descriptor,
                                 lowering_.builder_.CreateStructGEP(layout.type, object, 1));
  lowering_.builder_.CreateMemSet(
      lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), object, 16),
      lowering_.builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  llvm::Value *message = llvm::UndefValue::get(lowering_.strTy_);
  message = lowering_.builder_.CreateInsertValue(message, errorMaker_->getArg(1), 0);
  message = lowering_.builder_.CreateInsertValue(message, errorMaker_->getArg(2), 1);
  lowering_.builder_.CreateStore(message,
                                 lowering_.objectLayouts().fieldAddress(object, type, "message"));
  std::vector<llvm::Constant *> names;
  for (size_t k = 0; k < count; ++k) {
    names.push_back(lowering_.stringLowerer().stringLiteral(utf16(kErrorKinds[k])));
  }
  auto *namesType = llvm::ArrayType::get(lowering_.strTy_, count);
  auto *namesTable = new llvm::GlobalVariable(
      *lowering_.module_, namesType, true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantArray::get(namesType, names), "tz.error.names");
  llvm::Value *name = lowering_.builder_.CreateLoad(
      lowering_.strTy_, lowering_.builder_.CreateInBoundsGEP(
                            namesType, namesTable, {lowering_.builder_.getInt64(0), kind}));
  lowering_.builder_.CreateStore(name,
                                 lowering_.objectLayouts().fieldAddress(object, type, "name"));
  lowering_.builder_.CreateRet(object);
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return errorMaker_;
}

void ErrorLowerer::emitErrorSupport() {
  if (!lowering_.errorClass_) {
    return;
  }
  const Type *type = lowering_.errorClass_->type;
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
  const llvm::StructLayout *offsets =
      lowering_.module_->getDataLayout().getStructLayout(layout.type);
  auto offset = [&](llvm::StringRef field) {
    return lowering_.builder_.getInt32(uint32_t(
        offsets->getElementOffset(layout.elements[ObjectLayouts::fieldIndex(type, field)])));
  };
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  auto *infoType = llvm::StructType::get(lowering_.ctx_, {ptr, ptr, i32, i32, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      infoType, {errorMaker(), lowering_.typeDescriptors().typeDescriptor(Type::longTy()),
                 offset("name"), offset("message"), offset("cause"), offset("#frames")});
  auto *info = new llvm::GlobalVariable(*lowering_.module_, infoType, true,
                                        llvm::GlobalValue::PrivateLinkage, init, "tz.error.info");
  lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
                                    "tz_error_init", lowering_.builder_.getVoidTy(), {ptr}),
                                {info});
}

llvm::Value *ErrorLowerer::emitNewError(NewExpr *expr) {
  int kind = std::max(errorKindIndex(expr->cls->name), 0);
  const CallExpr *ctor = expr->ctor;
  const FuncSymbol *sym = ctor->func;
  std::vector<llvm::Value *> args = lowering_.throwLowerer().emitHeldValues(ctor->args);
  for (size_t i = args.size(); i < sym->params.size(); ++i) {
    args.push_back(lowering_.optionalLowerer().undefinedValue(sym->params[i]));
  }
  const Type *type = lowering_.errorClass_->type;
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object = lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                                      {lowering_.builder_.getInt64(size)}, "error");
  lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                 lowering_.builder_.CreateStructGEP(layout.type, object, 0));
  lowering_.builder_.CreateStore(errorDescriptor(kind),
                                 lowering_.builder_.CreateStructGEP(layout.type, object, 1));
  lowering_.builder_.CreateMemSet(
      lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), object, 16),
      lowering_.builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  args.insert(args.begin(), object);
  lowering_.builder_.CreateCall(lowering_.classDescriptors().methodCode(sym->decl), args);
  if (kind != 0) {
    lowering_.builder_.CreateStore(
        lowering_.stringLowerer().stringLiteral(utf16(kErrorKinds[kind])),
        lowering_.objectLayouts().fieldAddress(object, type, "name"));
  }

  return object;
}

llvm::Value *ErrorLowerer::emitErrorBuiltin(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  Lowering::Operand error = lowering_.refCounts().emitOperand(member->object, true);
  llvm::Type *ptr = lowering_.builder_.getPtrTy();
  const char *name = call->builtin == Builtin::ErrorStack ? "tz_error_stack" : "tz_error_to_string";
  llvm::Value *text = lowering_.stringLowerer().callReturningString(
      lowering_.processLowerer().processFunction(name, lowering_.builder_.getVoidTy(), {ptr, ptr}),
      {error.value});
  lowering_.refCounts().dropOperand(error);

  return text;
}

}
