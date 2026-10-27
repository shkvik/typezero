#include "compiler/library/bytes/bytes_call_site/bytes_call_site.h"
#include "compiler/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include <numeric>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

int64_t constantOf(const Expr *e) { return int64_t(cast<IntLitExpr>(e)->value); }

bool isConstant(const Expr *e) { return isa<IntLitExpr>(e) || isa<BoolLitExpr>(e); }

llvm::StructType *bytesStruct(llvm::LLVMContext &ctx) {
  llvm::Type *ptr = llvm::PointerType::getUnqual(ctx);
  llvm::Type *i32 = llvm::Type::getInt32Ty(ctx), *i64 = llvm::Type::getInt64Ty(ctx);

  return llvm::StructType::get(ctx, {i64, ptr, ptr, i32, i32, ptr});
}

bool isSignedKind(Bytes kind) {
  return kind == Bytes::Int8 || kind == Bytes::Int16 || kind == Bytes::Int32;
}

BytesCallSite::BytesCallSite(Lowering &lowering, CallExpr *call)
    : lowering_(lowering), call(call), member(dyn_cast<MemberExpr>(call->callee)), type(call->type),
      voidTy(lowering.builder_.getVoidTy()), i32(lowering.builder_.getInt32Ty()),
      i64(lowering.builder_.getInt64Ty()), f64(lowering.builder_.getDoubleTy()),
      ptr(lowering.builder_.getPtrTy()) {}

llvm::FunctionCallee BytesCallSite::fn(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params) {
  return lowering_.processLowerer().processFunction(name, ret, params);
}

llvm::ConstantInt *BytesCallSite::kindOf(const Type *t) {
  return lowering_.builder_.getInt32(uint32_t(t->bytes()));
}

Lowering::Operand BytesCallSite::receiver(size_t from) {
  auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
  Lowering::Operand self = lowering_.refCounts().emitOperand(
      member->object, llvm::all_of(rest, AssignLowerer::cannotAssign));

  return self;
}

llvm::Value *BytesCallSite::returnSelf(Lowering::Operand &self) {
  if (!self.owned) {
    lowering_.refCounts().retainValue(self.value, self.type);
  }
  self.owned = false;

  return self.value;
}

llvm::Value *BytesCallSite::positions(size_t from, size_t count,
                                      std::vector<llvm::Value *> &values) {
  uint32_t has = 0;
  for (size_t k = 0; k < count; ++k) {
    if (from + k < call->args.size()) {
      values.push_back(lowering_.exprLowerer().emitExpr(call->args[from + k]));
      has |= 1u << k;
    } else {
      values.push_back(lowering_.builder_.getInt64(0));
    }
  }

  return lowering_.builder_.getInt32(has);
}

llvm::Value *BytesCallSite::encoding(Expr *arg) {
  if (auto *lit = dyn_cast<IntLitExpr>(arg)) {
    return lowering_.builder_.getInt32(uint32_t(lit->value));
  }
  Lowering::Operand name = lowering_.refCounts().emitOperand(arg, true);
  llvm::Value *value =
      lowering_.builder_.CreateCall(fn("tz_buffer_encoding_checked", i32, {ptr, i64}),
                                    {lowering_.stringLowerer().stringObj(name.value),
                                     lowering_.stringLowerer().stringMeta(name.value)});
  lowering_.refCounts().dropOperand(name);

  return value;
}

}
