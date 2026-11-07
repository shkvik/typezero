#include "compiler/lower/expr/unary_lowerer/unary_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

static std::u16string ascii16(llvm::StringRef text) {
  return std::u16string(text.begin(), text.end());
}

UnaryLowerer::UnaryLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *UnaryLowerer::emitUnary(UnaryExpr *unary) {
  if (unary->op == UnaryOp::TypeOf) {
    return emitTypeOf(unary->operand);
  }
  if (unary->op == UnaryOp::Void) {
    lowering_.stmtLowerer().emitDiscarded(unary->operand);
    return lowering_.builder_.getFalse();
  }
  const Type *type = unary->operand->type;
  if ((unary->op == UnaryOp::Plus || unary->op == UnaryOp::Neg) && type->isString()) {
    Lowering::Operand str = lowering_.refCounts().emitOperand(unary->operand, true);
    llvm::Value *number = lowering_.numberLowerer().emitStringToNumber(str.value);
    lowering_.refCounts().dropOperand(str);
    return unary->op == UnaryOp::Neg ? lowering_.builder_.CreateFNeg(number) : number;
  }
  if (type->isDecimal()) {
    if (unary->op == UnaryOp::Plus) {
      return lowering_.exprLowerer().emitExpr(unary->operand);
    }
    if (llvm::isa<IntLitExpr>(unary->operand) || llvm::isa<FloatLitExpr>(unary->operand)) {
      return lowering_.decimalLowerer().emitDecimalLiteral(unary);
    }
    Lowering::Operand value = lowering_.refCounts().emitOperand(unary->operand, true);
    llvm::Value *negated = lowering_.decimalLowerer().callReturningDecimal(
        "tz_dec_neg", {lowering_.decimalLowerer().decimalCoef(value.value),
                       lowering_.decimalLowerer().decimalMeta(value.value)});
    lowering_.refCounts().dropOperand(value);
    return negated;
  }
  llvm::Value *operand = lowering_.exprLowerer().emitExpr(unary->operand);
  switch (unary->op) {
  case UnaryOp::Neg:
    if (type->isBool()) {
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
    return lowering_.builder_.CreateNot(operand);
  case UnaryOp::BitNot:
    return lowering_.builder_.CreateNot(operand);
  case UnaryOp::TypeOf:
  case UnaryOp::NonNull:
  case UnaryOp::Void:
  case UnaryOp::Delete:
    break;
  }
  llvm_unreachable("unknown unary operator");
}

llvm::Value *UnaryLowerer::emitTypeOf(Expr *operand) {
  const Type *type = operand->type;
  if (type->nonOptional()->isJson()) {
    return lowering_.jsonLowerer().emitJsonTypeOf(operand);
  }
  if (type->isUnknown() || type->isAnyObject()) {
    return lowering_.unknownLowerer().emitUnknownTypeOf(operand);
  }
  if (type->nonOptional()->isTagged()) {
    Lowering::Operand value = lowering_.refCounts().emitOperand(operand, true);
    llvm::Value *name = lowering_.unionLowerer().emitUnionTypeOf(value.value, type->nonOptional());
    lowering_.refCounts().dropOperand(value);
    return name;
  }
  if (type->isOptional()) {
    Lowering::Operand value = lowering_.refCounts().emitOperand(operand, true);
    llvm::Value *has = lowering_.optionalLowerer().optionalHas(value.value, type);
    lowering_.refCounts().dropOperand(value);
    return lowering_.builder_.CreateSelect(
        has, lowering_.stringLowerer().stringLiteral(ascii16(typeOfName(type->inner()))),
        lowering_.stringLowerer().stringLiteral(u"undefined"));
  }
  const Type *plain = type->nonOptional();
  bool classes = plain->isUnion() && llvm::StringRef(typeOfName(plain)).empty() &&
                 llvm::any_of(plain->members(), [](const Type *m) { return m->isClassRef(); }) &&
                 llvm::all_of(plain->members(), [](const Type *m) {
                   return m->isClassRef() || llvm::StringRef(typeOfName(m)) == "object";
                 });
  if (classes) {
    constexpr uint32_t kLayoutClass = 2097152;
    Lowering::Operand value = lowering_.refCounts().emitOperand(operand, true);
    llvm::Value *layout =
        lowering_.builder_.CreateLoad(lowering_.builder_.getPtrTy(),
                                      lowering_.builder_.CreateConstInBoundsGEP1_64(
                                          lowering_.builder_.getInt8Ty(), value.value, 8),
                                      "layout");
    llvm::Value *flags = lowering_.builder_.CreateLoad(
        lowering_.builder_.getInt32Ty(),
        lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, layout, 3), "flags");
    llvm::Value *isClass = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateAnd(flags, lowering_.builder_.getInt32(kLayoutClass)),
        lowering_.builder_.getInt32(0));
    lowering_.refCounts().dropOperand(value);
    return lowering_.builder_.CreateSelect(isClass,
                                           lowering_.stringLowerer().stringLiteral(u"class"),
                                           lowering_.stringLowerer().stringLiteral(u"object"));
  }
  if (!isa<NameExpr>(operand)) {
    lowering_.stmtLowerer().emitDiscarded(operand);
  }

  return lowering_.stringLowerer().stringLiteral(ascii16(typeOfName(type)));
}

llvm::Value *UnaryLowerer::emitEnumName(IndexExpr *index) {
  llvm::Value *value = lowering_.exprLowerer().emitExpr(index->index);
  const Type *type = index->type;
  llvm::Value *result = lowering_.optionalLowerer().undefinedValue(type);
  for (const EnumMember &m : index->enumDecl()->members) {
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

}
