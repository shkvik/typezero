#include "compiler/lower/values/cstring_lowerer/cstring_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/program/to_utf8/to_utf8.h"

namespace tz {

namespace {

constexpr uint64_t kForeverCount = uint64_t(1) << 61;
constexpr uint64_t kTextOffset = 24;

}

CStringLowerer::CStringLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *CStringLowerer::emitLiteral(const std::u16string &text) {
  auto bytes = ToUtf8::toUtf8(text);
  llvm::Constant *&found = this->literals[bytes];
  if (found != nullptr) {
    return found;
  }
  auto *i64 = this->lowering.builder.getInt64Ty();
  auto *ptr = this->lowering.builder.getPtrTy();
  auto *data = llvm::ConstantDataArray::getString(this->lowering.ctx, bytes, true);
  auto *literalTy = llvm::StructType::get(this->lowering.ctx, {i64, ptr, i64, data->getType()});
  auto *layout =
      this->lowering.module_->getOrInsertGlobal("tz_cstring_layout", this->lowering.layoutDescTy);
  auto *count = this->lowering.builder.getInt64(kForeverCount);
  auto *length = this->lowering.builder.getInt64(bytes.size());
  auto *init = llvm::ConstantStruct::get(literalTy, {count, layout, length, data});
  found = new llvm::GlobalVariable(*this->lowering.module_, literalTy, false,
                                   llvm::GlobalValue::PrivateLinkage, init, "tz.cstring");

  return found;
}

llvm::Value *CStringLowerer::emitFromString(llvm::Value *text) {
  auto *ptr = this->lowering.builder.getPtrTy();
  auto *i64 = this->lowering.builder.getInt64Ty();
  auto callee = this->runtimeFunction("tz_cstring_from_str", ptr, {ptr, i64});
  auto *object = this->lowering.stringLowerer().stringObj(text);
  auto *meta = this->lowering.stringLowerer().stringMeta(text);

  return this->lowering.builder.CreateCall(callee, {object, meta});
}

llvm::Value *CStringLowerer::emitToString(llvm::Value *cstring) {
  auto *ptr = this->lowering.builder.getPtrTy();
  auto callee =
      this->runtimeFunction("tz_cstring_to_str", this->lowering.builder.getVoidTy(), {ptr, ptr});

  return this->lowering.stringLowerer().callReturningString(callee, {cstring});
}

llvm::Value *CStringLowerer::emitToStringCall(CallExpr *call) {
  auto *member = llvm::cast<MemberExpr>(call->callee);
  auto receiver = this->lowering.refCounts().emitOperand(member->object, true);
  auto *text = this->emitToString(receiver.value);
  this->lowering.refCounts().dropOperand(receiver);

  return text;
}

llvm::Value *CStringLowerer::emitEqual(llvm::Value *first, llvm::Value *second) {
  auto *ptr = this->lowering.builder.getPtrTy();
  auto *i32 = this->lowering.builder.getInt32Ty();
  auto callee = this->runtimeFunction("tz_cstring_equal", i32, {ptr, ptr});
  auto *equal = this->lowering.builder.CreateCall(callee, {first, second});

  return this->lowering.builder.CreateICmpNE(equal, this->lowering.builder.getInt32(0));
}

llvm::Value *CStringLowerer::emitCopy(llvm::Value *pointer) {
  auto *ptr = this->lowering.builder.getPtrTy();
  auto callee = this->runtimeFunction("tz_cstring_copy", ptr, {ptr});

  return this->lowering.builder.CreateCall(callee, {pointer});
}

llvm::Value *CStringLowerer::textPointer(llvm::Value *cstring) {
  return this->lowering.builder.CreateConstInBoundsGEP1_64(this->lowering.builder.getInt8Ty(),
                                                           cstring, kTextOffset, "cstring.text");
}

llvm::FunctionCallee CStringLowerer::runtimeFunction(llvm::StringRef name, llvm::Type *result,
                                                     llvm::ArrayRef<llvm::Type *> params) {
  auto *type = llvm::FunctionType::get(result, params, false);

  return this->lowering.module_->getOrInsertFunction(name, type);
}

}
