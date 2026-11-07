#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr uint64_t kStrLengthMask = (uint64_t(1) << 30) - 1;

constexpr uint64_t kStrTwoByte = uint64_t(1) << 61;

constexpr uint64_t kStrRope = uint64_t(1) << 62;

constexpr uint64_t kStrHeap = uint64_t(1) << 63;

}

bool EqualityLowerer::isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

EqualityLowerer::EqualityLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *EqualityLowerer::emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type) {
  type = UnionLayout::storedType(type);
  if (UnionLayout::isAtomType(type) || UnionLayout::isObjectUnion(type)) {
    return this->lowering.builder_.CreateICmpEQ(a, b);
  }
  if (type->isTagged()) {
    return this->lowering.unionLowerer().emitUnionEqual(a, b, type);
  }
  if (type->nonOptional()->isJson()) {
    llvm::Type *ptr = this->lowering.builder_.getPtrTy();
    llvm::Value *equal = this->lowering.builder_.CreateCall(
        this->lowering.jsonLowerer().jsonFunction("tz_json_strict_equal",
                                                  this->lowering.builder_.getInt32Ty(), {ptr, ptr}),
        {a, b});
    return this->lowering.builder_.CreateICmpNE(equal, this->lowering.builder_.getInt32(0));
  }
  switch (type->kind()) {
  case Type::Kind::String: {
    if (isa<llvm::Constant>(a)) {
      std::swap(a, b);
    }
    auto *literal = dyn_cast<llvm::Constant>(b);
    if (auto *meta =
            literal ? llvm::dyn_cast_or_null<llvm::ConstantInt>(literal->getAggregateElement(1u))
                    : nullptr) {
      uint64_t bits = meta->getZExtValue();
      if ((bits & kStrLengthMask) == 0) {
        return this->lowering.builder_.CreateICmpEQ(this->lowering.stringLowerer().stringLength(a),
                                                    this->lowering.builder_.getInt64(0));
      }
      if ((bits & (kStrRope | kStrTwoByte | kStrHeap)) == kStrRope) {
        return this->lowering.builder_.CreateAnd(
            this->lowering.builder_.CreateICmpEQ(this->lowering.stringLowerer().stringObj(a),
                                                 this->lowering.stringLowerer().stringObj(b)),
            this->lowering.builder_.CreateICmpEQ(this->lowering.stringLowerer().stringMeta(a),
                                                 this->lowering.stringLowerer().stringMeta(b)));
      }
    }
    llvm::Value *args[] = {
        this->lowering.stringLowerer().stringObj(a), this->lowering.stringLowerer().stringMeta(a),
        this->lowering.stringLowerer().stringObj(b), this->lowering.stringLowerer().stringMeta(b)};
    return this->lowering.builder_.CreateICmpNE(
        this->lowering.builder_.CreateCall(this->lowering.rt_.strEqual, args),
        this->lowering.builder_.getInt32(0));
  }
  case Type::Kind::Array:
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Bool:
  case Type::Kind::ClassRef:
    return this->lowering.builder_.CreateICmpEQ(a, b);
  case Type::Kind::Unknown: {
    llvm::Type *ptr = this->lowering.builder_.getPtrTy();
    llvm::Value *equal = this->lowering.builder_.CreateCall(
        this->lowering.unknownLowerer().unknownFunction(
            "tz_unknown_equal", this->lowering.builder_.getInt32Ty(), {ptr, ptr}),
        {a, b});
    return this->lowering.builder_.CreateICmpNE(equal, this->lowering.builder_.getInt32(0));
  }
  case Type::Kind::Decimal: {
    llvm::Type *i64 = this->lowering.builder_.getInt64Ty();
    llvm::Value *equal = this->lowering.builder_.CreateCall(
        this->lowering.decimalLowerer().decimalFunction(
            "tz_dec_equal", this->lowering.builder_.getInt32Ty(), {i64, i64, i64, i64}),
        {this->lowering.decimalLowerer().decimalCoef(a),
         this->lowering.decimalLowerer().decimalMeta(a),
         this->lowering.decimalLowerer().decimalCoef(b),
         this->lowering.decimalLowerer().decimalMeta(b)});
    return this->lowering.builder_.CreateICmpNE(equal, this->lowering.builder_.getInt32(0));
  }
  case Type::Kind::Function:
    return this->lowering.builder_.CreateAnd(
        this->lowering.builder_.CreateICmpEQ(this->lowering.builder_.CreateExtractValue(a, 0),
                                             this->lowering.builder_.CreateExtractValue(b, 0)),
        this->lowering.builder_.CreateICmpEQ(this->lowering.builder_.CreateExtractValue(a, 1),
                                             this->lowering.builder_.CreateExtractValue(b, 1)));
  case Type::Kind::Optional: {
    if (EqualityLowerer::isReference(type->inner()) || UnionLayout::isAtomType(type->inner())) {
      return this->lowering.builder_.CreateICmpEQ(a, b);
    }
    if (type->inner()->isTagged()) {
      return this->lowering.unionLowerer().emitUnionEqual(a, b, type->inner());
    }
    llvm::Value *hasA = this->lowering.builder_.CreateExtractValue(a, 1),
                *hasB = this->lowering.builder_.CreateExtractValue(b, 1);
    llvm::Value *values =
        this->emitValuesEqual(this->lowering.builder_.CreateExtractValue(a, 0),
                              this->lowering.builder_.CreateExtractValue(b, 0), type->inner());
    llvm::Value *both = this->lowering.builder_.CreateAnd(hasA, hasB);
    llvm::Value *neither =
        this->lowering.builder_.CreateNot(this->lowering.builder_.CreateOr(hasA, hasB));
    return this->lowering.builder_.CreateOr(neither,
                                            this->lowering.builder_.CreateAnd(both, values));
  }
  default:
    return this->lowering.binaryLowerer().emitComparison(BinaryOp::Eq, a, b, type);
  }
}

llvm::Value *EqualityLowerer::emitEquality(BinaryExpr *bin) {
  bool isEq = bin->op == BinaryOp::Eq;
  Expr *lhs = bin->lhs;
  Expr *rhs = bin->rhs;
  if (lhs->type->nonOptional()->isJson() || rhs->type->nonOptional()->isJson()) {
    return this->lowering.jsonLowerer().emitJsonEquality(bin);
  }
  if (lhs->type->isUndefined() || rhs->type->isUndefined()) {
    if (lhs->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(lhs);
    }
    Expr *other = lhs->type->isUndefined() ? rhs : lhs;
    if (other->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(rhs);
      return this->lowering.builder_.getInt1(isEq);
    }
    Lowering::Operand value = this->lowering.refCounts().emitOperand(
        other, other == rhs || AssignLowerer::cannotAssign(rhs));
    if (rhs->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(rhs);
    }
    llvm::Value *has = other->type->isOptional() || other->type->isUnknown()
                           ? this->lowering.optionalLowerer().optionalHas(value.value, other->type)
                           : this->lowering.builder_.getTrue();
    this->lowering.refCounts().dropOperand(value);
    return isEq ? this->lowering.builder_.CreateNot(has) : has;
  }
  if (lhs->type->isString() && !lhs->type->isOptional()) {
    return this->lowering.stringLowerer().emitStringComparison(bin);
  }

  if (lhs->type->isInteger() && rhs->type->isInteger() && lhs->type != rhs->type) {
    llvm::Value *a = this->lowering.exprLowerer().emitExpr(lhs);
    llvm::Value *b = this->lowering.exprLowerer().emitExpr(rhs);

    return this->lowering.binaryLowerer().emitMixedComparison(bin->op, a, b, lhs->type, rhs->type);
  }
  Lowering::Operand a =
      this->lowering.refCounts().emitOperand(lhs, AssignLowerer::cannotAssign(rhs));
  Lowering::Operand b = this->lowering.refCounts().emitOperand(rhs, true);
  llvm::Value *equal = this->emitValuesEqual(a.value, b.value, lhs->type);
  this->lowering.refCounts().dropOperand(a);
  this->lowering.refCounts().dropOperand(b);

  return isEq ? equal : this->lowering.builder_.CreateNot(equal);
}

llvm::Value *EqualityLowerer::emitNullish(BinaryExpr *bin) {
  const Type *lhsType = bin->lhs->type;
  if (lhsType->nonOptional()->isJson()) {
    return this->lowering.jsonLowerer().emitJsonNullish(bin);
  }
  if (lhsType->isUndefined()) {
    this->lowering.stmtLowerer().emitDiscarded(bin->lhs);
    return this->lowering.exprLowerer().emitExpr(bin->rhs);
  }
  if (!lhsType->isOptional() && !lhsType->isUnknown()) {
    return this->lowering.exprLowerer().emitExpr(bin->lhs);
  }

  llvm::Value *lhs = this->lowering.exprLowerer().emitExpr(bin->lhs);
  llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("nullish.value");
  llvm::BasicBlock *defaultBlock = this->lowering.functionFrame().newBlock("nullish.default");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("nullish.end");
  this->lowering.builder_.CreateCondBr(this->lowering.optionalLowerer().optionalHas(lhs, lhsType),
                                       valueBlock, defaultBlock);

  this->lowering.functionFrame().switchTo(valueBlock);
  llvm::Value *value =
      bin->type->isOptional() ? lhs : this->lowering.optionalLowerer().optionalValue(lhs, lhsType);
  llvm::BasicBlock *valueEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(defaultBlock);
  llvm::Value *fallback = this->lowering.exprLowerer().emitExpr(bin->rhs);
  llvm::BasicBlock *defaultEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi =
      this->lowering.builder_.CreatePHI(this->lowering.moduleBuilder().lower(bin->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(fallback, defaultEnd);

  return phi;
}

}
