#include "compiler/lower/library/bytes/bytes_call_site/bytes_call_site.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/bytes/bytes_lower_helpers/bytes_lower_helpers.h"
#include "compiler/lower/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include "llvm/IR/MDBuilder.h"
#include <numeric>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

BytesCallSite::BytesCallSite(Lowering &lowering, CallExpr *call)
    : lowering(lowering),
      call(call),
      member(dyn_cast<MemberExpr>(call->callee)),
      type(call->type),
      voidTy(lowering.builder.getVoidTy()),
      i32(lowering.builder.getInt32Ty()),
      i64(lowering.builder.getInt64Ty()),
      f64(lowering.builder.getDoubleTy()),
      ptr(lowering.builder.getPtrTy()) {}

int64_t BytesCallSite::constantOf(const Expr *e) {
  return int64_t(cast<IntLitExpr>(e)->value);
}

llvm::FunctionCallee BytesCallSite::fn(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params) {
  return this->lowering.processLowerer().processFunction(name, ret, params);
}

llvm::ConstantInt *BytesCallSite::kindOf(const Type *t) {
  return this->lowering.builder.getInt32(uint32_t(t->bytes()));
}

Lowering::Operand BytesCallSite::receiver(size_t from) {
  auto rest = llvm::ArrayRef<Expr *>(call->args).drop_front(std::min(from, call->args.size()));
  Lowering::Operand self = this->lowering.refCounts().emitOperand(
      member->object, llvm::all_of(rest, AssignLowerer::cannotAssign));

  return self;
}

llvm::Value *BytesCallSite::returnSelf(Lowering::Operand &self) {
  if (!self.owned) {
    this->lowering.refCounts().retainValue(self.value, self.type);
  }
  self.owned = false;

  return self.value;
}

llvm::Value *BytesCallSite::positions(size_t from, size_t count,
                                      std::vector<llvm::Value *> &values) {
  uint32_t has = 0;
  for (size_t k = 0; k < count; ++k) {
    if (from + k < call->args.size()) {
      values.push_back(this->lowering.exprLowerer().emitExpr(call->args[from + k]));
      has |= 1u << k;
    } else {
      values.push_back(this->lowering.builder.getInt64(0));
    }
  }

  return this->lowering.builder.getInt32(has);
}

llvm::Value *BytesCallSite::encoding(Expr *arg) {
  if (auto *lit = dyn_cast<IntLitExpr>(arg)) {
    return this->lowering.builder.getInt32(uint32_t(lit->value));
  }
  Lowering::Operand name = this->lowering.refCounts().emitOperand(arg, true);
  llvm::Value *value =
      this->lowering.builder.CreateCall(this->fn("tz_buffer_encoding_checked", i32, {ptr, i64}),
                                        {this->lowering.stringLowerer().stringObj(name.value),
                                         this->lowering.stringLowerer().stringMeta(name.value)});
  this->lowering.refCounts().dropOperand(name);

  return value;
}

}
