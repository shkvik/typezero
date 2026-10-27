#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
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

constexpr uint64_t kStrLengthMask = (uint64_t(1) << 30) - 1;

constexpr uint64_t kStrAscii = uint64_t(1) << 60;

constexpr uint64_t kStrTwoByte = uint64_t(1) << 61;

constexpr uint64_t kStrRope = uint64_t(1) << 62;

constexpr size_t kInlineMax = 8;

constexpr uint64_t kNumberPart = kStrRope | kStrTwoByte;

constexpr uint64_t kNumberInt = 1;

constexpr uint64_t kNumberDouble = 2;

constexpr uint32_t kKindBuffer1 = 1;

constexpr uint32_t kKindBuffer2 = 2;

constexpr int64_t kLiteralRefCount = int64_t(1) << 62;

void collectConcat(Expr *expr, std::vector<Expr *> &parts) {
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (bin && bin->op == BinaryOp::Add && bin->type->isString()) {
    collectConcat(bin->lhs, parts);
    collectConcat(bin->rhs, parts);
  } else {
    parts.push_back(expr);
  }
}

}

StringLowerer::StringLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Constant *StringLowerer::utf8Constant(llvm::StringRef text) {
  auto [it, inserted] = utf8Constants_.try_emplace(text, nullptr);
  if (inserted) {
    it->second = lowering_.builder_.CreateGlobalString(text, ".msg", 0, lowering_.module_.get());
  }

  return it->second;
}

llvm::Value *StringLowerer::stringObj(llvm::Value *str) {
  return lowering_.builder_.CreateExtractValue(str, 0);
}

llvm::Value *StringLowerer::stringMeta(llvm::Value *str) {
  return lowering_.builder_.CreateExtractValue(str, 1);
}

llvm::Value *StringLowerer::stringLength(llvm::Value *str) {
  return lowering_.builder_.CreateAnd(stringMeta(str), lowering_.builder_.getInt64(kStrLengthMask),
                                      "length");
}

llvm::Value *StringLowerer::callReturningString(llvm::FunctionCallee fn,
                                                llvm::ArrayRef<llvm::Value *> args) {
  llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "str.out");
  std::vector<llvm::Value *> all{out};
  all.insert(all.end(), args.begin(), args.end());
  lowering_.builder_.CreateCall(fn, all);

  return lowering_.builder_.CreateLoad(lowering_.strTy_, out);
}

llvm::Value *StringLowerer::concatOperands(llvm::ArrayRef<Lowering::Operand> operands) {
  auto *arrayTy = llvm::ArrayType::get(lowering_.strTy_, operands.size());
  llvm::AllocaInst *array = lowering_.functionFrame().createStackSlot(arrayTy, "concat.parts");
  for (size_t i = 0; i < operands.size(); ++i) {
    lowering_.builder_.CreateStore(
        operands[i].value,
        lowering_.builder_.CreateConstInBoundsGEP2_32(arrayTy, array, 0, static_cast<unsigned>(i)));
  }
  llvm::Value *result = callReturningString(lowering_.rt_.strConcatN,
                                            {array, lowering_.builder_.getInt64(operands.size())});
  for (const Lowering::Operand &operand : operands) {
    lowering_.refCounts().dropOperand(operand);
  }

  return result;
}

llvm::Value *StringLowerer::emitConcat(BinaryExpr *bin) {
  std::vector<Expr *> parts;
  collectConcat(bin, parts);
  std::vector<bool> laterCannotAssign(parts.size(), true);
  for (size_t i = parts.size() - 1; i > 0; --i) {
    laterCannotAssign[i - 1] = laterCannotAssign[i] && AssignLowerer::cannotAssign(parts[i]);
  }
  std::vector<Lowering::Operand> operands;
  for (size_t i = 0; i < parts.size(); ++i) {
    operands.push_back(emitConcatPart(parts[i], laterCannotAssign[i]));
  }

  return concatOperands(operands);
}

Lowering::Operand StringLowerer::emitConcatPart(Expr *expr, bool canBorrow) {
  auto *conv = dyn_cast<CastExpr>(expr);
  const Type *from = conv && conv->isImplicit() ? conv->operand->type : nullptr;
  if (!from || !(from->isInteger() || from->isFloat())) {
    return lowering_.refCounts().emitOperand(expr, canBorrow);
  }
  llvm::Value *value = lowering_.exprLowerer().emitExpr(conv->operand);
  llvm::Value *bits = from->isFloat()
                          ? lowering_.builder_.CreateBitCast(value, lowering_.builder_.getInt64Ty())
                          : lowering_.builder_.CreateSExt(value, lowering_.builder_.getInt64Ty());
  llvm::Value *part = llvm::UndefValue::get(lowering_.strTy_);
  part = lowering_.builder_.CreateInsertValue(
      part, lowering_.builder_.CreateIntToPtr(bits, lowering_.builder_.getPtrTy()), 0);
  part = lowering_.builder_.CreateInsertValue(
      part,
      lowering_.builder_.getInt64(kNumberPart | (from->isFloat() ? kNumberDouble : kNumberInt)), 1);

  return {part, false, Type::string()};
}

llvm::Value *StringLowerer::emitTemplate(TemplateExpr *tmpl) {
  size_t count = tmpl->exprs.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i) {
    laterCannotAssign[i - 2] =
        laterCannotAssign[i - 1] && AssignLowerer::cannotAssign(tmpl->exprs[i - 1]);
  }

  if (count == 1 && tmpl->quasis[0].empty() && tmpl->quasis[1].empty()) {
    return lowering_.exprLowerer().emitExpr(tmpl->exprs[0]);
  }
  std::vector<Lowering::Operand> operands;
  for (size_t i = 0; i <= count; ++i) {
    if (!tmpl->quasis[i].empty()) {
      operands.push_back({stringLiteral(tmpl->quasis[i]), false, Type::string()});
    }
    if (i < count) {
      operands.push_back(emitConcatPart(tmpl->exprs[i], laterCannotAssign[i]));
    }
  }
  if (operands.empty()) {
    return stringLiteral(u"");
  }
  if (operands.size() == 1) {
    return operands[0].value;
  }

  return concatOperands(operands);
}

llvm::Value *StringLowerer::emitStringComparison(BinaryExpr *bin) {
  Lowering::Operand lhs =
      lowering_.refCounts().emitOperand(bin->lhs, AssignLowerer::cannotAssign(bin->rhs));
  Lowering::Operand rhs = lowering_.refCounts().emitOperand(bin->rhs, true);
  llvm::Value *args[] = {stringObj(lhs.value), stringMeta(lhs.value), stringObj(rhs.value),
                         stringMeta(rhs.value)};
  llvm::Value *zero = lowering_.builder_.getInt32(0);
  llvm::Value *result;
  switch (bin->op) {
  case BinaryOp::Eq:
    result = lowering_.equalityLowerer().emitValuesEqual(lhs.value, rhs.value, Type::string());
    break;
  case BinaryOp::Ne:
    result = lowering_.builder_.CreateNot(
        lowering_.equalityLowerer().emitValuesEqual(lhs.value, rhs.value, Type::string()));
    break;
  case BinaryOp::Lt:
    result = lowering_.builder_.CreateICmpSLT(
        lowering_.builder_.CreateCall(lowering_.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Le:
    result = lowering_.builder_.CreateICmpSLE(
        lowering_.builder_.CreateCall(lowering_.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Gt:
    result = lowering_.builder_.CreateICmpSGT(
        lowering_.builder_.CreateCall(lowering_.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Ge:
    result = lowering_.builder_.CreateICmpSGE(
        lowering_.builder_.CreateCall(lowering_.rt_.strCompare, args), zero);
    break;
  default:
    llvm_unreachable("not a string comparison");
  }
  lowering_.refCounts().dropOperand(lhs);
  lowering_.refCounts().dropOperand(rhs);

  return result;
}

llvm::Value *StringLowerer::emitStringIndex(IndexExpr *index) {
  Lowering::Operand object =
      lowering_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
  llvm::Value *position = lowering_.exprLowerer().emitExpr(index->index);
  auto read = [&] {
    return callReturningString(lowering_.rt_.strCharAt,
                               {stringObj(object.value), stringMeta(object.value), position});
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange) {
    result = read();
  } else {
    result = lowering_.arrayLowerer().emitCheckedRead(
        lowering_.builder_.CreateICmpULT(position, stringLength(object.value)), index->type, read);
  }
  lowering_.refCounts().dropOperand(object);

  return result;
}

llvm::Constant *StringLowerer::stringLiteral(const std::u16string &text) {
  auto found = literals_.find(text);
  if (found != literals_.end()) {
    return found->second;
  }

  bool twoByte = llvm::any_of(text, [](char16_t c) { return c > 0xFF; });
  bool ascii = llvm::all_of(text, [](char16_t c) { return c < 0x80; });
  llvm::Constant *value;
  if (text.empty()) {
    value = llvm::ConstantStruct::get(
        lowering_.strTy_, {llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
                           lowering_.builder_.getInt64(kStrAscii)});
  } else if (!twoByte && text.size() <= kInlineMax) {
    bool little = lowering_.module_->getDataLayout().isLittleEndian();
    uint64_t bits = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      bits |= uint64_t(static_cast<uint8_t>(text[i])) << (8 * (little ? i : 7 - i));
    }
    uint64_t meta = text.size() | kStrRope | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(
        lowering_.strTy_, {llvm::ConstantExpr::getIntToPtr(lowering_.builder_.getInt64(bits),
                                                           lowering_.builder_.getPtrTy()),
                           lowering_.builder_.getInt64(meta)});
  } else {
    llvm::Constant *data;
    if (twoByte) {
      std::vector<uint16_t> units(text.begin(), text.end());
      data = llvm::ConstantDataArray::get(lowering_.ctx_, units);
    } else {
      std::vector<uint8_t> units;
      for (char16_t c : text) {
        units.push_back(static_cast<uint8_t>(c));
      }
      data = llvm::ConstantDataArray::get(lowering_.ctx_, units);
    }
    llvm::Type *i32 = lowering_.builder_.getInt32Ty();
    auto *layout = llvm::StructType::get(
        lowering_.ctx_, {lowering_.builder_.getInt64Ty(), i32, i32, i32, i32, data->getType()});
    auto length = static_cast<uint32_t>(text.size());
    llvm::Constant *init = llvm::ConstantStruct::get(
        layout, {lowering_.builder_.getInt64(kLiteralRefCount),
                 lowering_.builder_.getInt32(twoByte ? kKindBuffer2 : kKindBuffer1),
                 lowering_.builder_.getInt32(0), lowering_.builder_.getInt32(length),
                 lowering_.builder_.getInt32(length), data});
    auto *global = new llvm::GlobalVariable(*lowering_.module_, layout, true,
                                            llvm::GlobalValue::PrivateLinkage, init, ".str");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(8));
    uint64_t meta = length | (twoByte ? kStrTwoByte : 0) | (ascii ? kStrAscii : 0);
    value =
        llvm::ConstantStruct::get(lowering_.strTy_, {global, lowering_.builder_.getInt64(meta)});
  }
  literals_.emplace(text, value);

  return value;
}

}
