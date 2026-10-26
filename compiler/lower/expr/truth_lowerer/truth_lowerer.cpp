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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

bool isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

}

TruthLowerer::TruthLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *TruthLowerer::emitLogical(BinaryExpr *bin) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *lhsType = bin->lhs->type;
  llvm::Value *lhs = lowering_.exprLowerer().emitExpr(bin->lhs);
  llvm::Value *truth = emitTruth(lhs, lhsType);
  llvm::Value *decided = decidedValue(bin, lhs);
  llvm::BasicBlock *lhsEnd = lowering_.builder_.GetInsertBlock();
  llvm::BasicBlock *rhsBlock = lowering_.functionFrame().newBlock(isAnd ? "and.rhs" : "or.rhs");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock(isAnd ? "and.end" : "or.end");
  if (isAnd) {
    lowering_.builder_.CreateCondBr(truth, rhsBlock, endBlock);
  } else {
    lowering_.builder_.CreateCondBr(truth, endBlock, rhsBlock);
  }

  lowering_.functionFrame().switchTo(rhsBlock);
  lowering_.refCounts().releaseValue(lhs, lhsType);
  llvm::Value *rhs = lowering_.exprLowerer().emitExpr(bin->rhs);
  llvm::BasicBlock *rhsEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(bin->type), 2);
  phi->addIncoming(decided, lhsEnd);
  phi->addIncoming(rhs, rhsEnd);

  return phi;
}

llvm::Value *TruthLowerer::decidedValue(BinaryExpr *bin, llvm::Value *lhs) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *from = bin->lhs->type, *to = bin->type;
  if (from->isBool() && to->isBool()) {
    return lowering_.builder_.getInt1(!isAnd);
  }
  const Type *base = from->nonOptional();
  if (isAnd && (base->isArray() || base->isFunction() || isReference(base))) {
    return lowering_.optionalLowerer().undefinedValue(to);
  }
  if (from == to) {
    return lhs;
  }
  if (from->isOptional()) {
    return lowering_.optionalLowerer().optionalValue(lhs, from);
  }

  return lowering_.optionalLowerer().makeOptional(lhs, lowering_.builder_.getTrue(), to);
}

llvm::Value *TruthLowerer::emitTruth(llvm::Value *value, const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Bool:
    return value;
  case Type::Kind::Literal:
  case Type::Kind::Union:
    return lowering_.unionLowerer().emitUnionTruth(value, type);
  case Type::Kind::String:
    return lowering_.builder_.CreateICmpNE(lowering_.stringLowerer().stringLength(value),
                                           lowering_.builder_.getInt64(0));
  case Type::Kind::Optional: {
    llvm::Value *has = lowering_.optionalLowerer().optionalHas(value, type);
    const Type *inner = type->inner();
    if (!inner->isBool() && !inner->isString() && !inner->isLiteral() &&
        !(inner->isUnion() && !UnionLayout::isObjectUnion(inner))) {
      return has;
    }
    return lowering_.builder_.CreateAnd(
        has, emitTruth(lowering_.optionalLowerer().optionalValue(value, type), inner));
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
  Lowering::Operand value = lowering_.refCounts().emitOperand(expr, true);
  llvm::Value *truth = emitTruth(value.value, value.type);
  lowering_.refCounts().dropOperand(value);

  return truth;
}

}
