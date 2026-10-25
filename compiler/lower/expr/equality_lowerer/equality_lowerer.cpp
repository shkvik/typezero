#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// String value layout; must match runtime/core/string/string.c.
constexpr uint64_t kStrLengthMask = (uint64_t(1) << 30) - 1;

constexpr uint64_t kStrTwoByte = uint64_t(1) << 61;

constexpr uint64_t kStrRope = uint64_t(1) << 62;

constexpr uint64_t kStrHeap = uint64_t(1) << 63;

// A value held as a pointer, null for undefined in `T | undefined`: an
// object, an iterator, a Json, a dictionary or a Promise.
bool isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

} // namespace

EqualityLowerer::EqualityLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *EqualityLowerer::emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type) {
  type = UnionLayout::storedType(type);
  // Atoms are numbers, a union of objects a reference.
  if (UnionLayout::isAtomType(type) || UnionLayout::isObjectUnion(type)) {
    return codegen_.builder_.CreateICmpEQ(a, b);
  }
  if (type->isTagged()) {
    return codegen_.unionLowerer().emitUnionEqual(a, b, type);
  }
  if (type->nonOptional()->isJson()) { // as `===`, which a switch uses
    llvm::Type *ptr = codegen_.builder_.getPtrTy();
    llvm::Value *equal = codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_strict_equal", codegen_.builder_.getInt32Ty(), {ptr, ptr}), {a, b});
    return codegen_.builder_.CreateICmpNE(equal, codegen_.builder_.getInt32(0));
  }
  switch (type->kind()) {
  case Type::Kind::String: {
    // Comparing with a literal needs no call when the literal is empty or
    // inline: every one-byte string that short is inline, so equal ones have
    // equal words.
    if (isa<llvm::Constant>(a)) {
      std::swap(a, b);
    }
    auto *literal = dyn_cast<llvm::Constant>(b);
    if (auto *meta =
            literal ? llvm::dyn_cast_or_null<llvm::ConstantInt>(literal->getAggregateElement(1u))
                    : nullptr) {
      uint64_t bits = meta->getZExtValue();
      if ((bits & kStrLengthMask) == 0) {
        return codegen_.builder_.CreateICmpEQ(codegen_.stringLowerer().stringLength(a), codegen_.builder_.getInt64(0));
      }
      if ((bits & (kStrRope | kStrTwoByte | kStrHeap)) == kStrRope) {
        return codegen_.builder_.CreateAnd(codegen_.builder_.CreateICmpEQ(codegen_.stringLowerer().stringObj(a), codegen_.stringLowerer().stringObj(b)),
                                  codegen_.builder_.CreateICmpEQ(codegen_.stringLowerer().stringMeta(a), codegen_.stringLowerer().stringMeta(b)));
      }
    }
    llvm::Value *args[] = {codegen_.stringLowerer().stringObj(a), codegen_.stringLowerer().stringMeta(a), codegen_.stringLowerer().stringObj(b), codegen_.stringLowerer().stringMeta(b)};
    return codegen_.builder_.CreateICmpNE(codegen_.builder_.CreateCall(codegen_.rt_.strEqual, args), codegen_.builder_.getInt32(0));
  }
  case Type::Kind::Array:  // the same array
  case Type::Kind::Object: // the same object
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Bool:
  case Type::Kind::ClassRef:
    return codegen_.builder_.CreateICmpEQ(a, b);
  case Type::Kind::Unknown: { // as `===`
    llvm::Type *ptr = codegen_.builder_.getPtrTy();
    llvm::Value *equal = codegen_.builder_.CreateCall(
        codegen_.unknownLowerer().unknownFunction("tz_unknown_equal", codegen_.builder_.getInt32Ty(), {ptr, ptr}), {a, b});
    return codegen_.builder_.CreateICmpNE(equal, codegen_.builder_.getInt32(0));
  }
  case Type::Kind::Decimal: { // the same value: 0.30 == 0.3
    llvm::Type *i64 = codegen_.builder_.getInt64Ty();
    llvm::Value *equal = codegen_.builder_.CreateCall(
        codegen_.decimalLowerer().decimalFunction("tz_dec_equal", codegen_.builder_.getInt32Ty(), {i64, i64, i64, i64}),
        {codegen_.decimalLowerer().decimalCoef(a), codegen_.decimalLowerer().decimalMeta(a), codegen_.decimalLowerer().decimalCoef(b), codegen_.decimalLowerer().decimalMeta(b)});
    return codegen_.builder_.CreateICmpNE(equal, codegen_.builder_.getInt32(0));
  }
  case Type::Kind::Function: // the same function with the same environment
    return codegen_.builder_.CreateAnd(
        codegen_.builder_.CreateICmpEQ(codegen_.builder_.CreateExtractValue(a, 0), codegen_.builder_.CreateExtractValue(b, 0)),
        codegen_.builder_.CreateICmpEQ(codegen_.builder_.CreateExtractValue(a, 1),
                              codegen_.builder_.CreateExtractValue(b, 1)));
  case Type::Kind::Optional: {
    if (isReference(type->inner()) || UnionLayout::isAtomType(type->inner())) { // both null, or the same object
      return codegen_.builder_.CreateICmpEQ(a, b);
    }
    if (type->inner()->isTagged()) { // a tag 0 is undefined
      return codegen_.unionLowerer().emitUnionEqual(a, b, type->inner());
    }
    llvm::Value *hasA = codegen_.builder_.CreateExtractValue(a, 1),
                *hasB = codegen_.builder_.CreateExtractValue(b, 1);
    llvm::Value *values = emitValuesEqual(codegen_.builder_.CreateExtractValue(a, 0),
                                          codegen_.builder_.CreateExtractValue(b, 0), type->inner());
    // Both undefined, or both defined and equal. The zero values of undefined
    // are valid, so comparing them is harmless.
    llvm::Value *both = codegen_.builder_.CreateAnd(hasA, hasB);
    llvm::Value *neither = codegen_.builder_.CreateNot(codegen_.builder_.CreateOr(hasA, hasB));
    return codegen_.builder_.CreateOr(neither, codegen_.builder_.CreateAnd(both, values));
  }
  default:
    return codegen_.binaryLowerer().emitComparison(BinaryOp::Eq, a, b, type);
  }
}

// `==` and `!=`. Sema converted both sides to one type, unless one of them
// is `undefined`.
llvm::Value *EqualityLowerer::emitEquality(BinaryExpr *bin) {
  bool isEq = bin->op == BinaryOp::Eq;
  Expr *lhs = bin->lhs, *rhs = bin->rhs;
  if (lhs->type->nonOptional()->isJson() || rhs->type->nonOptional()->isJson()) {
    return codegen_.jsonLowerer().emitJsonEquality(bin);
  }
  if (lhs->type->isUndefined() || rhs->type->isUndefined()) {
    // A side that is always undefined, such as `void f()`, still runs, in
    // its turn.
    if (lhs->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(lhs);
    }
    Expr *other = lhs->type->isUndefined() ? rhs : lhs;
    if (other->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(rhs);
      return codegen_.builder_.getInt1(isEq);
    }
    CodeGen::Operand value = codegen_.refCounts().emitOperand(other, /*canBorrow=*/other == rhs || AssignLowerer::cannotAssign(rhs));
    if (rhs->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(rhs);
    }
    llvm::Value *has = other->type->isOptional() || other->type->isUnknown()
                           ? codegen_.optionalLowerer().optionalHas(value.value, other->type)
                           : codegen_.builder_.getTrue();
    codegen_.refCounts().dropOperand(value);
    return isEq ? codegen_.builder_.CreateNot(has) : has;
  }
  if (lhs->type->isString() && !lhs->type->isOptional()) {
    return codegen_.stringLowerer().emitStringComparison(bin);
  }

  CodeGen::Operand a = codegen_.refCounts().emitOperand(lhs, AssignLowerer::cannotAssign(rhs));
  codegen_.throwLowerer().holdOperand(a, codegen_.mayThrow().exprThrows(rhs));
  CodeGen::Operand b = codegen_.refCounts().emitOperand(rhs, /*canBorrow=*/true);
  codegen_.throwLowerer().takeOperand(a);
  llvm::Value *equal = emitValuesEqual(a.value, b.value, lhs->type);
  codegen_.refCounts().dropOperand(a);
  codegen_.refCounts().dropOperand(b);

  return isEq ? equal : codegen_.builder_.CreateNot(equal);
}

// `a ?? b`: b is evaluated only when a is undefined.
llvm::Value *EqualityLowerer::emitNullish(BinaryExpr *bin) {
  const Type *lhsType = bin->lhs->type;
  if (lhsType->nonOptional()->isJson()) {
    return codegen_.jsonLowerer().emitJsonNullish(bin);
  }
  if (lhsType->isUndefined()) {
    codegen_.stmtLowerer().emitDiscarded(bin->lhs); // `void f() ?? x` still runs f
    return codegen_.exprLowerer().emitExpr(bin->rhs);
  }
  if (!lhsType->isOptional() && !lhsType->isUnknown()) { // never undefined
    return codegen_.exprLowerer().emitExpr(bin->lhs);
  }

  llvm::Value *lhs = codegen_.exprLowerer().emitExpr(bin->lhs);
  llvm::BasicBlock *valueBlock = codegen_.functionFrame().newBlock("nullish.value");
  llvm::BasicBlock *defaultBlock = codegen_.functionFrame().newBlock("nullish.default");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("nullish.end");
  codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(lhs, lhsType), valueBlock, defaultBlock);

  codegen_.functionFrame().switchTo(valueBlock);
  llvm::Value *value = bin->type->isOptional() ? lhs : codegen_.optionalLowerer().optionalValue(lhs, lhsType);
  llvm::BasicBlock *valueEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(defaultBlock); // lhs is undefined and holds nothing to release
  llvm::Value *fallback = codegen_.exprLowerer().emitExpr(bin->rhs);
  llvm::BasicBlock *defaultEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(bin->type), 2);
  phi->addIncoming(value, valueEnd);
  phi->addIncoming(fallback, defaultEnd);

  return phi;
}

} // namespace tz
