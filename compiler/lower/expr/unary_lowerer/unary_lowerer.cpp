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

using llvm::isa;

namespace tz {

static std::u16string ascii16(llvm::StringRef text) {
  return std::u16string(text.begin(), text.end());
}

UnaryLowerer::UnaryLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *UnaryLowerer::emitUnary(UnaryExpr *unary) {
  if (unary->op == UnaryOp::TypeOf) {
    return this->emitTypeOf(unary->operand);
  }
  if (unary->op == UnaryOp::Void) {
    this->lowering.stmtLowerer().emitDiscarded(unary->operand);
    return this->lowering.builder_.getFalse();
  }
  const Type *type = unary->operand->type;
  if ((unary->op == UnaryOp::Plus || unary->op == UnaryOp::Neg) && type->isString()) {
    Lowering::Operand str = this->lowering.refCounts().emitOperand(unary->operand, true);
    llvm::Value *number = this->lowering.numberLowerer().emitStringToNumber(str.value);
    this->lowering.refCounts().dropOperand(str);
    return unary->op == UnaryOp::Neg ? this->lowering.builder_.CreateFNeg(number) : number;
  }
  if (type->isDecimal()) {
    if (unary->op == UnaryOp::Plus) {
      return this->lowering.exprLowerer().emitExpr(unary->operand);
    }
    if (llvm::isa<IntLitExpr>(unary->operand) || llvm::isa<FloatLitExpr>(unary->operand)) {
      return this->lowering.decimalLowerer().emitDecimalLiteral(unary);
    }
    Lowering::Operand value = this->lowering.refCounts().emitOperand(unary->operand, true);
    llvm::Value *negated = this->lowering.decimalLowerer().callReturningDecimal(
        "tz_dec_neg", {this->lowering.decimalLowerer().decimalCoef(value.value),
                       this->lowering.decimalLowerer().decimalMeta(value.value)});
    this->lowering.refCounts().dropOperand(value);
    return negated;
  }
  llvm::Value *operand = this->lowering.exprLowerer().emitExpr(unary->operand);
  switch (unary->op) {
  case UnaryOp::Neg:
    if (type->isBool()) {
      return this->lowering.builder_.CreateFNeg(
          this->lowering.builder_.CreateUIToFP(operand, this->lowering.builder_.getDoubleTy()));
    }
    return UnionLayout::storedType(unary->type)->isFloat()
               ? this->lowering.builder_.CreateFNeg(operand)
               : this->lowering.builder_.CreateNeg(operand);
  case UnaryOp::Plus:
    return type->isBool() ? this->lowering.builder_.CreateUIToFP(
                                operand, this->lowering.builder_.getDoubleTy())
                          : operand;
  case UnaryOp::Not:
    return this->lowering.builder_.CreateNot(operand);
  case UnaryOp::BitNot:
    return this->lowering.builder_.CreateNot(operand);
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
    return this->lowering.jsonLowerer().emitJsonTypeOf(operand);
  }
  if (type->isUnknown() || type->isAnyObject()) {
    return this->lowering.unknownLowerer().emitUnknownTypeOf(operand);
  }
  if (type->nonOptional()->isTagged()) {
    Lowering::Operand value = this->lowering.refCounts().emitOperand(operand, true);
    llvm::Value *name =
        this->lowering.unionLowerer().emitUnionTypeOf(value.value, type->nonOptional());
    this->lowering.refCounts().dropOperand(value);
    return name;
  }
  if (type->isOptional()) {
    Lowering::Operand value = this->lowering.refCounts().emitOperand(operand, true);
    llvm::Value *has = this->lowering.optionalLowerer().optionalHas(value.value, type);
    this->lowering.refCounts().dropOperand(value);
    return this->lowering.builder_.CreateSelect(
        has, this->lowering.stringLowerer().stringLiteral(ascii16(typeOfName(type->inner()))),
        this->lowering.stringLowerer().stringLiteral(u"undefined"));
  }
  const Type *plain = type->nonOptional();
  bool classes = plain->isUnion() && llvm::StringRef(typeOfName(plain)).empty() &&
                 llvm::any_of(plain->members(), [](const Type *m) { return m->isClassRef(); }) &&
                 llvm::all_of(plain->members(), [](const Type *m) {
                   return m->isClassRef() || llvm::StringRef(typeOfName(m)) == "object";
                 });
  if (classes) {
    constexpr uint32_t kLayoutClass = 2097152;
    Lowering::Operand value = this->lowering.refCounts().emitOperand(operand, true);
    llvm::Value *layout =
        this->lowering.builder_.CreateLoad(this->lowering.builder_.getPtrTy(),
                                           this->lowering.builder_.CreateConstInBoundsGEP1_64(
                                               this->lowering.builder_.getInt8Ty(), value.value, 8),
                                           "layout");
    llvm::Value *flags = this->lowering.builder_.CreateLoad(
        this->lowering.builder_.getInt32Ty(),
        this->lowering.builder_.CreateStructGEP(this->lowering.layoutDescTy_, layout, 3), "flags");
    llvm::Value *isClass = this->lowering.builder_.CreateICmpNE(
        this->lowering.builder_.CreateAnd(flags, this->lowering.builder_.getInt32(kLayoutClass)),
        this->lowering.builder_.getInt32(0));
    this->lowering.refCounts().dropOperand(value);
    return this->lowering.builder_.CreateSelect(
        isClass, this->lowering.stringLowerer().stringLiteral(u"class"),
        this->lowering.stringLowerer().stringLiteral(u"object"));
  }
  if (!isa<NameExpr>(operand)) {
    this->lowering.stmtLowerer().emitDiscarded(operand);
  }

  return this->lowering.stringLowerer().stringLiteral(ascii16(typeOfName(type)));
}

llvm::Value *UnaryLowerer::emitEnumName(IndexExpr *index) {
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(index->index);
  const Type *type = index->type;
  llvm::Value *result = this->lowering.optionalLowerer().undefinedValue(type);
  for (const EnumMember &m : index->enumDecl()->members) {
    llvm::SmallVector<llvm::UTF16, 16> units;
    llvm::convertUTF8ToUTF16String(m.name, units);
    llvm::Value *name = this->lowering.optionalLowerer().makeOptional(
        this->lowering.stringLowerer().stringLiteral(std::u16string(units.begin(), units.end())),
        this->lowering.builder_.getTrue(), type);
    llvm::Value *matches = this->lowering.builder_.CreateICmpEQ(
        value, this->lowering.builder_.getInt64(static_cast<uint64_t>(int64_t(m.number))));
    result = this->lowering.builder_.CreateSelect(matches, name, result);
  }

  return result;
}

}
