#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool TruthLowerer::isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

TruthLowerer::TruthLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *TruthLowerer::emitLogical(BinaryExpr *bin) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *lhsType = bin->lhs->type;
  llvm::Value *lhs = this->lowering.exprLowerer().emitExpr(bin->lhs);
  llvm::Value *truth = this->emitTruth(lhs, lhsType);
  llvm::Value *decided = this->decidedValue(bin, lhs);
  llvm::BasicBlock *lhsEnd = this->lowering.builder.GetInsertBlock();
  llvm::BasicBlock *rhsBlock =
      this->lowering.functionFrame().newBlock(isAnd ? "and.rhs" : "or.rhs");
  llvm::BasicBlock *endBlock =
      this->lowering.functionFrame().newBlock(isAnd ? "and.end" : "or.end");
  if (isAnd) {
    this->lowering.builder.CreateCondBr(truth, rhsBlock, endBlock);
  } else {
    this->lowering.builder.CreateCondBr(truth, endBlock, rhsBlock);
  }

  this->lowering.functionFrame().switchTo(rhsBlock);
  this->lowering.refCounts().releaseValue(lhs, lhsType);
  llvm::Value *rhs = this->lowering.exprLowerer().emitExpr(bin->rhs);
  llvm::BasicBlock *rhsEnd = this->lowering.builder.GetInsertBlock();
  this->lowering.builder.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi =
      this->lowering.builder.CreatePHI(this->lowering.moduleBuilder().lower(bin->type), 2);
  phi->addIncoming(decided, lhsEnd);
  phi->addIncoming(rhs, rhsEnd);

  return phi;
}

llvm::Value *TruthLowerer::decidedValue(BinaryExpr *bin, llvm::Value *lhs) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *from = bin->lhs->type;
  const Type *to = bin->type;
  if (from->isBool() && to->isBool()) {
    return this->lowering.builder.getInt1(!isAnd);
  }
  const Type *base = from->nonOptional();
  if (isAnd && (base->isArray() || base->isFunction() || TruthLowerer::isReference(base))) {
    return this->lowering.optionalLowerer().undefinedValue(to);
  }
  if (from == to) {
    return lhs;
  }
  if (from->isOptional()) {
    return this->lowering.optionalLowerer().optionalValue(lhs, from);
  }

  return this->lowering.optionalLowerer().makeOptional(lhs, this->lowering.builder.getTrue(), to);
}

llvm::Value *TruthLowerer::emitTruth(llvm::Value *value, const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Bool:
    return value;
  case Type::Kind::Literal:
  case Type::Kind::Union:
    return this->lowering.unionLowerer().emitUnionTruth(value, type);
  case Type::Kind::String:
    return this->lowering.builder.CreateICmpNE(this->lowering.stringLowerer().stringLength(value),
                                               this->lowering.builder.getInt64(0));
  case Type::Kind::Optional: {
    llvm::Value *has = this->lowering.optionalLowerer().optionalHas(value, type);
    const Type *inner = type->inner();
    if (!inner->isBool() && !inner->isString() && !inner->isLiteral() &&
        !(inner->isUnion() && !UnionLayout::isObjectUnion(inner))) {
      return has;
    }
    return this->lowering.builder.CreateAnd(
        has, this->emitTruth(this->lowering.optionalLowerer().optionalValue(value, type), inner));
  }
  default:
    llvm_unreachable("Checker rejects testing other types");
  }
}

llvm::Value *TruthLowerer::emitTruthOf(Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && !conv->shortCircuit && isa<NameExpr>(conv->operand) &&
      conv->operand->type->isOptional() && conv->type == conv->operand->type->inner()) {
    expr = conv->operand;
  }
  Lowering::Operand value = this->lowering.refCounts().emitOperand(expr, true);
  llvm::Value *truth = this->emitTruth(value.value, value.type);
  this->lowering.refCounts().dropOperand(value);

  return truth;
}

}
