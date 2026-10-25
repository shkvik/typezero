#include "compiler/lower/expr/unary_lowerer/unary_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
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

// The text of a name or a field read through names, for messages; empty for
// anything else.
static std::string valueText(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return "";
    }
    expr = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    std::string object = valueText(member->object);
    return object.empty() ? "" : object + (member->optional ? "?." : ".") + member->name.str();
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    std::string object = valueText(index->object), position = valueText(index->index);
    if (auto *lit = dyn_cast<IntLitExpr>(index->index)) {
      position = std::to_string(lit->value);
    }
    return object.empty() || position.empty() ? "" : object + "[" + position + "]";
  }

  return "";
}

static std::u16string ascii16(llvm::StringRef text) {
  return std::u16string(text.begin(), text.end());
}

UnaryLowerer::UnaryLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *UnaryLowerer::emitUnary(UnaryExpr *unary) {
  if (unary->op == UnaryOp::TypeOf) {
    return emitTypeOf(unary->operand);
  }
  if (unary->op == UnaryOp::NonNull) {
    return emitNonNull(unary);
  }
  if (unary->op == UnaryOp::Void) {
    lowering_.stmtLowerer().emitDiscarded(unary->operand);
    return lowering_.builder_.getFalse(); // undefined, as for the literal
  }
  const Type *type = unary->operand->type;
  if ((unary->op == UnaryOp::Plus || unary->op == UnaryOp::Neg) && type->isString()) {
    // `+s` is Number(s), and `-s` is -Number(s).
    Lowering::Operand str = lowering_.refCounts().emitOperand(unary->operand, /*canBorrow=*/true);
    llvm::Value *number = lowering_.numberLowerer().emitStringToNumber(str.value);
    lowering_.refCounts().dropOperand(str);
    return unary->op == UnaryOp::Neg ? lowering_.builder_.CreateFNeg(number) : number;
  }
  if (type->isDecimal()) { // Checker allows only `-x` and `+literal`
    if (unary->op == UnaryOp::Plus) {
      return lowering_.exprLowerer().emitExpr(unary->operand);
    }
    if (llvm::isa<IntLitExpr>(unary->operand) || llvm::isa<FloatLitExpr>(unary->operand)) {
      return lowering_.decimalLowerer().emitDecimalLiteral(unary);
    }
    Lowering::Operand value = lowering_.refCounts().emitOperand(unary->operand, /*canBorrow=*/true);
    llvm::Value *negated = lowering_.decimalLowerer().callReturningDecimal(
        "tz_dec_neg", {lowering_.decimalLowerer().decimalCoef(value.value),
                       lowering_.decimalLowerer().decimalMeta(value.value)});
    lowering_.refCounts().dropOperand(value);
    return negated;
  }
  llvm::Value *operand = lowering_.exprLowerer().emitExpr(unary->operand);
  switch (unary->op) {
  case UnaryOp::Neg:
    if (type->isBool()) { // `-true` is -1
      return lowering_.builder_.CreateFNeg(
          lowering_.builder_.CreateUIToFP(operand, lowering_.builder_.getDoubleTy()));
    }
    return UnionLayout::storedType(unary->type)->isFloat() ? lowering_.builder_.CreateFNeg(operand)
                                                           : lowering_.builder_.CreateNeg(operand);
  case UnaryOp::Plus:
    return type->isBool()
               ? lowering_.builder_.CreateUIToFP(operand, lowering_.builder_.getDoubleTy())
               : operand;
  case UnaryOp::Not:
  case UnaryOp::BitNot:
    return lowering_.builder_.CreateNot(operand);
  case UnaryOp::TypeOf:
  case UnaryOp::NonNull:
  case UnaryOp::Void:
  case UnaryOp::Delete: // Checker rejects it for now
    break;
  }
  llvm_unreachable("unknown unary operator");
}

// `x!`: TypeError if x is undefined (undefined holds nothing to release).
llvm::Value *UnaryLowerer::emitNonNull(UnaryExpr *unary) {
  const Type *type = unary->operand->type;
  llvm::Value *value = lowering_.exprLowerer().emitExpr(unary->operand);
  if (!type->isOptional()) {
    return value;
  }
  llvm::BasicBlock *throwBlock = lowering_.functionFrame().newBlock("nonnull.undefined");
  llvm::BasicBlock *okBlock = lowering_.functionFrame().newBlock("nonnull.ok");
  lowering_.builder_.CreateCondBr(
      lowering_.builder_.CreateNot(lowering_.optionalLowerer().optionalHas(value, type)),
      throwBlock, okBlock, llvm::MDBuilder(lowering_.ctx_).createUnlikelyBranchWeights());
  lowering_.functionFrame().switchTo(throwBlock);
  std::string text = valueText(unary->operand);
  lowering_.throwLowerer().emitThrowNew(1, (text.empty() ? "the value" : text) + " is undefined");
  lowering_.functionFrame().switchTo(okBlock);

  return lowering_.optionalLowerer().optionalValue(value, type);
}

// `typeof x` follows from the type of x, except for `T | undefined`.
llvm::Value *UnaryLowerer::emitTypeOf(Expr *operand) {
  const Type *type = operand->type;
  if (type->nonOptional()->isJson()) {
    return lowering_.jsonLowerer().emitJsonTypeOf(operand);
  }
  if (type->isUnknown() || type->isAnyObject()) {
    return lowering_.unknownLowerer().emitUnknownTypeOf(operand);
  }
  // A tagged union: by its tag, which is 0 for undefined.
  if (type->nonOptional()->isTagged()) {
    Lowering::Operand value = lowering_.refCounts().emitOperand(operand, /*canBorrow=*/true);
    llvm::Value *name = lowering_.unionLowerer().emitUnionTypeOf(value.value, type->nonOptional());
    lowering_.refCounts().dropOperand(value);
    return name;
  }
  if (type->isOptional()) {
    Lowering::Operand value = lowering_.refCounts().emitOperand(operand, /*canBorrow=*/true);
    llvm::Value *has = lowering_.optionalLowerer().optionalHas(value.value, type);
    lowering_.refCounts().dropOperand(value);
    return lowering_.builder_.CreateSelect(
        has, lowering_.stringLowerer().stringLiteral(ascii16(typeOfName(type->inner()))),
        lowering_.stringLowerer().stringLiteral(u"undefined"));
  }
  if (!isa<NameExpr>(operand)) { // evaluated for its side effects
    lowering_.stmtLowerer().emitDiscarded(operand);
  }

  return lowering_.stringLowerer().stringLiteral(ascii16(typeOfName(type)));
}

// `Color[value]`: the name of the enum member with that value, or undefined.
llvm::Value *UnaryLowerer::emitEnumName(IndexExpr *index) {
  llvm::Value *value = lowering_.exprLowerer().emitExpr(index->index);
  const Type *type = index->type;
  llvm::Value *result = lowering_.optionalLowerer().undefinedValue(type);
  // Each member's test wraps those of the members before it, so that of
  // members with one value the last names it, as in JavaScript.
  for (const EnumMember &m : index->enumDecl->members) {
    llvm::SmallVector<llvm::UTF16, 16> units;
    llvm::convertUTF8ToUTF16String(m.name, units);
    llvm::Value *name = lowering_.optionalLowerer().makeOptional(
        lowering_.stringLowerer().stringLiteral(std::u16string(units.begin(), units.end())),
        lowering_.builder_.getTrue(), type);
    llvm::Value *matches = lowering_.builder_.CreateICmpEQ(
        value, lowering_.builder_.getInt64(static_cast<uint64_t>(int64_t(m.number))));
    result = lowering_.builder_.CreateSelect(matches, name, result);
  }

  return result;
}

} // namespace tz
