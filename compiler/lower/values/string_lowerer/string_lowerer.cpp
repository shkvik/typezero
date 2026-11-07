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
#include "llvm/Support/ErrorHandling.h"

using llvm::dyn_cast;

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

}

void StringLowerer::collectConcat(Expr *expr, std::vector<Expr *> &parts) {
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (bin && bin->op == BinaryOp::Add && bin->type->isString()) {
    StringLowerer::collectConcat(bin->lhs, parts);
    StringLowerer::collectConcat(bin->rhs, parts);
  } else {
    parts.push_back(expr);
  }
}

StringLowerer::StringLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Constant *StringLowerer::utf8Constant(llvm::StringRef text) {
  auto [it, inserted] = this->utf8Constants.try_emplace(text, nullptr);
  if (inserted) {
    it->second =
        this->lowering.builder_.CreateGlobalString(text, ".msg", 0, this->lowering.module_.get());
  }

  return it->second;
}

llvm::Value *StringLowerer::stringObj(llvm::Value *str) {
  return this->lowering.builder_.CreateExtractValue(str, 0);
}

llvm::Value *StringLowerer::stringMeta(llvm::Value *str) {
  return this->lowering.builder_.CreateExtractValue(str, 1);
}

llvm::Value *StringLowerer::stringLength(llvm::Value *str) {
  return this->lowering.builder_.CreateAnd(
      this->stringMeta(str), this->lowering.builder_.getInt64(kStrLengthMask), "length");
}

llvm::Value *StringLowerer::callReturningString(llvm::FunctionCallee fn,
                                                llvm::ArrayRef<llvm::Value *> args) {
  llvm::AllocaInst *out =
      this->lowering.functionFrame().createStackSlot(this->lowering.strTy_, "str.out");
  std::vector<llvm::Value *> all{out};
  all.insert(all.end(), args.begin(), args.end());
  this->lowering.builder_.CreateCall(fn, all);

  return this->lowering.builder_.CreateLoad(this->lowering.strTy_, out);
}

llvm::Value *StringLowerer::concatOperands(llvm::ArrayRef<Lowering::Operand> operands) {
  auto *arrayTy = llvm::ArrayType::get(this->lowering.strTy_, operands.size());
  llvm::AllocaInst *array = this->lowering.functionFrame().createStackSlot(arrayTy, "concat.parts");
  for (size_t i = 0; i < operands.size(); ++i) {
    this->lowering.builder_.CreateStore(operands[i].value,
                                        this->lowering.builder_.CreateConstInBoundsGEP2_32(
                                            arrayTy, array, 0, static_cast<unsigned>(i)));
  }
  llvm::Value *result = this->callReturningString(
      this->lowering.rt_.strConcatN, {array, this->lowering.builder_.getInt64(operands.size())});
  for (const Lowering::Operand &operand : operands) {
    this->lowering.refCounts().dropOperand(operand);
  }

  return result;
}

llvm::Value *StringLowerer::emitConcat(BinaryExpr *bin) {
  std::vector<Expr *> parts;
  StringLowerer::collectConcat(bin, parts);
  std::vector<bool> laterCannotAssign(parts.size(), true);
  for (size_t i = parts.size() - 1; i > 0; --i) {
    laterCannotAssign[i - 1] = laterCannotAssign[i] && AssignLowerer::cannotAssign(parts[i]);
  }
  std::vector<Lowering::Operand> operands;
  for (size_t i = 0; i < parts.size(); ++i) {
    operands.push_back(this->emitConcatPart(parts[i], laterCannotAssign[i]));
  }

  return this->concatOperands(operands);
}

Lowering::Operand StringLowerer::emitConcatPart(Expr *expr, bool canBorrow) {
  auto *conv = dyn_cast<CastExpr>(expr);
  const Type *from = conv && conv->isImplicit() ? conv->operand->type : nullptr;
  if (!from || !(from->isInteger() || from->isFloat()) ||
      (from->isInteger() && from->isUnsigned() && from->intMax() > uint64_t(INT64_MAX))) {
    return this->lowering.refCounts().emitOperand(expr, canBorrow);
  }
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(conv->operand);
  llvm::Value *bits =
      from->isFloat()
          ? this->lowering.builder_.CreateBitCast(value, this->lowering.builder_.getInt64Ty())
      : from->isUnsigned()
          ? this->lowering.builder_.CreateZExt(value, this->lowering.builder_.getInt64Ty())
          : this->lowering.builder_.CreateSExt(value, this->lowering.builder_.getInt64Ty());
  llvm::Value *part = llvm::UndefValue::get(this->lowering.strTy_);
  part = this->lowering.builder_.CreateInsertValue(
      part, this->lowering.builder_.CreateIntToPtr(bits, this->lowering.builder_.getPtrTy()), 0);
  part = this->lowering.builder_.CreateInsertValue(
      part,
      this->lowering.builder_.getInt64(kNumberPart |
                                       (from->isFloat() ? kNumberDouble : kNumberInt)),
      1);

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
    return this->lowering.exprLowerer().emitExpr(tmpl->exprs[0]);
  }
  std::vector<Lowering::Operand> operands;
  for (size_t i = 0; i <= count; ++i) {
    if (!tmpl->quasis[i].empty()) {
      operands.push_back({this->stringLiteral(tmpl->quasis[i]), false, Type::string()});
    }
    if (i < count) {
      operands.push_back(this->emitConcatPart(tmpl->exprs[i], laterCannotAssign[i]));
    }
  }
  if (operands.empty()) {
    return this->stringLiteral(u"");
  }
  if (operands.size() == 1) {
    return operands[0].value;
  }

  return this->concatOperands(operands);
}

llvm::Value *StringLowerer::emitStringComparison(BinaryExpr *bin) {
  Lowering::Operand lhs =
      this->lowering.refCounts().emitOperand(bin->lhs, AssignLowerer::cannotAssign(bin->rhs));
  Lowering::Operand rhs = this->lowering.refCounts().emitOperand(bin->rhs, true);
  llvm::Value *args[] = {this->stringObj(lhs.value), this->stringMeta(lhs.value),
                         this->stringObj(rhs.value), this->stringMeta(rhs.value)};
  llvm::Value *zero = this->lowering.builder_.getInt32(0);
  llvm::Value *result;
  switch (bin->op) {
  case BinaryOp::Eq:
    result = this->lowering.equalityLowerer().emitValuesEqual(lhs.value, rhs.value, Type::string());
    break;
  case BinaryOp::Ne:
    result = this->lowering.builder_.CreateNot(
        this->lowering.equalityLowerer().emitValuesEqual(lhs.value, rhs.value, Type::string()));
    break;
  case BinaryOp::Lt:
    result = this->lowering.builder_.CreateICmpSLT(
        this->lowering.builder_.CreateCall(this->lowering.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Le:
    result = this->lowering.builder_.CreateICmpSLE(
        this->lowering.builder_.CreateCall(this->lowering.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Gt:
    result = this->lowering.builder_.CreateICmpSGT(
        this->lowering.builder_.CreateCall(this->lowering.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Ge:
    result = this->lowering.builder_.CreateICmpSGE(
        this->lowering.builder_.CreateCall(this->lowering.rt_.strCompare, args), zero);
    break;
  default:
    llvm_unreachable("not a string comparison");
  }
  this->lowering.refCounts().dropOperand(lhs);
  this->lowering.refCounts().dropOperand(rhs);

  return result;
}

llvm::Value *StringLowerer::emitStringIndex(IndexExpr *index) {
  Lowering::Operand object = this->lowering.refCounts().emitOperand(
      index->object, AssignLowerer::cannotAssign(index->index));
  llvm::Value *position = this->lowering.exprLowerer().emitExpr(index->index);
  auto read = [&] {
    return this->callReturningString(
        this->lowering.rt_.strCharAt,
        {this->stringObj(object.value), this->stringMeta(object.value), position});
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange()) {
    result = read();
  } else {
    result = this->lowering.arrayLowerer().emitCheckedRead(
        this->lowering.builder_.CreateICmpULT(position, this->stringLength(object.value)),
        index->type, read);
  }
  this->lowering.refCounts().dropOperand(object);

  return result;
}

llvm::Constant *StringLowerer::stringLiteral(const std::u16string &text) {
  auto found = this->literals.find(text);
  if (found != this->literals.end()) {
    return found->second;
  }

  bool twoByte = llvm::any_of(text, [](char16_t c) { return c > 0xFF; });
  bool ascii = llvm::all_of(text, [](char16_t c) { return c < 0x80; });
  llvm::Constant *value;
  if (text.empty()) {
    value = llvm::ConstantStruct::get(
        this->lowering.strTy_, {llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()),
                                this->lowering.builder_.getInt64(kStrAscii)});
  } else if (!twoByte && text.size() <= kInlineMax) {
    bool little = this->lowering.module_->getDataLayout().isLittleEndian();
    uint64_t bits = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      bits |= uint64_t(static_cast<uint8_t>(text[i])) << (8 * (little ? i : 7 - i));
    }
    uint64_t meta = text.size() | kStrRope | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(
        this->lowering.strTy_,
        {llvm::ConstantExpr::getIntToPtr(this->lowering.builder_.getInt64(bits),
                                         this->lowering.builder_.getPtrTy()),
         this->lowering.builder_.getInt64(meta)});
  } else {
    llvm::Constant *data;
    if (twoByte) {
      std::vector<uint16_t> units(text.begin(), text.end());
      data = llvm::ConstantDataArray::get(this->lowering.ctx_, units);
    } else {
      std::vector<uint8_t> units;
      for (char16_t c : text) {
        units.push_back(static_cast<uint8_t>(c));
      }
      data = llvm::ConstantDataArray::get(this->lowering.ctx_, units);
    }
    llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
    auto *layout =
        llvm::StructType::get(this->lowering.ctx_, {this->lowering.builder_.getInt64Ty(), i32, i32,
                                                    i32, i32, data->getType()});
    auto length = static_cast<uint32_t>(text.size());
    llvm::Constant *init = llvm::ConstantStruct::get(
        layout, {this->lowering.builder_.getInt64(kLiteralRefCount),
                 this->lowering.builder_.getInt32(twoByte ? kKindBuffer2 : kKindBuffer1),
                 this->lowering.builder_.getInt32(0), this->lowering.builder_.getInt32(length),
                 this->lowering.builder_.getInt32(length), data});
    auto *global = new llvm::GlobalVariable(*this->lowering.module_, layout, true,
                                            llvm::GlobalValue::PrivateLinkage, init, ".str");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(8));
    uint64_t meta = length | (twoByte ? kStrTwoByte : 0) | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(this->lowering.strTy_,
                                      {global, this->lowering.builder_.getInt64(meta)});
  }
  this->literals.emplace(text, value);

  return value;
}

}
