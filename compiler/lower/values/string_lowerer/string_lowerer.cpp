#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
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

constexpr uint64_t kStrAscii = uint64_t(1) << 60;

constexpr uint64_t kStrTwoByte = uint64_t(1) << 61;

constexpr uint64_t kStrRope = uint64_t(1) << 62;

// A one-byte string of up to this many code units is stored in the value
// itself: `obj` holds the code units and `meta` has ROPE without HEAP.
constexpr size_t kInlineMax = 8;

// A number passed to tz_str_concat_n as it is (see
// runtime/core/string/string.c): the ROPE and TWO_BYTE flags without HEAP, and
// the kind of number in the low bits.
constexpr uint64_t kNumberPart = kStrRope | kStrTwoByte;

constexpr uint64_t kNumberInt = 1;

constexpr uint64_t kNumberDouble = 2;

constexpr uint32_t kKindBuffer1 = 1;

constexpr uint32_t kKindBuffer2 = 2;

// Literals are never counted (their values lack the HEAP flag); a huge count
// just makes them look alive to anyone inspecting memory.
constexpr int64_t kLiteralRefCount = int64_t(1) << 62;

// Collects the operands of a chain of string `+`. Concatenation is
// associative, so a + (b + c) flattens as well as (a + b) + c.
void collectConcat(Expr *expr, std::vector<Expr *> &parts) {
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (bin && bin->op == BinaryOp::Add && bin->type->isString()) {
    collectConcat(bin->lhs, parts);
    collectConcat(bin->rhs, parts);
  } else {
    parts.push_back(expr);
  }
}

} // namespace

StringLowerer::StringLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Constant *StringLowerer::utf8Constant(llvm::StringRef text) {
  auto [it, inserted] = utf8Constants_.try_emplace(text, nullptr);
  if (inserted) {
    it->second = codegen_.builder_.CreateGlobalString(text, ".msg", 0, codegen_.module_.get());
  }

  return it->second;
}

llvm::Value *StringLowerer::stringObj(llvm::Value *str) { return codegen_.builder_.CreateExtractValue(str, 0); }

llvm::Value *StringLowerer::stringMeta(llvm::Value *str) { return codegen_.builder_.CreateExtractValue(str, 1); }

llvm::Value *StringLowerer::stringLength(llvm::Value *str) {
  return codegen_.builder_.CreateAnd(stringMeta(str), codegen_.builder_.getInt64(kStrLengthMask), "length");
}

llvm::Value *StringLowerer::callReturningString(llvm::FunctionCallee fn,
                                          llvm::ArrayRef<llvm::Value *> args) {
  llvm::AllocaInst *out = codegen_.functionFrame().createStackSlot(codegen_.strTy_, "str.out");
  std::vector<llvm::Value *> all{out};
  all.insert(all.end(), args.begin(), args.end());
  codegen_.builder_.CreateCall(fn, all);

  return codegen_.builder_.CreateLoad(codegen_.strTy_, out);
}

llvm::Value *StringLowerer::concatOperands(llvm::ArrayRef<CodeGen::Operand> operands) {
  auto *arrayTy = llvm::ArrayType::get(codegen_.strTy_, operands.size());
  llvm::AllocaInst *array = codegen_.functionFrame().createStackSlot(arrayTy, "concat.parts");
  for (size_t i = 0; i < operands.size(); ++i) {
    codegen_.builder_.CreateStore(operands[i].value, codegen_.builder_.CreateConstInBoundsGEP2_32(
                                                arrayTy, array, 0, static_cast<unsigned>(i)));
  }
  llvm::Value *result =
      callReturningString(codegen_.rt_.strConcatN, {array, codegen_.builder_.getInt64(operands.size())});
  for (const CodeGen::Operand &operand : operands) {
    codegen_.refCounts().dropOperand(operand);
  }

  return result;
}

llvm::Value *StringLowerer::emitConcat(BinaryExpr *bin) {
  std::vector<Expr *> parts;
  collectConcat(bin, parts);
  // A part can be borrowed if nothing after it can reassign its variable.
  std::vector<bool> laterCannotAssign(parts.size(), true);
  for (size_t i = parts.size() - 1; i > 0; --i) {
    laterCannotAssign[i - 1] = laterCannotAssign[i] && AssignLowerer::cannotAssign(parts[i]);
  }

  std::vector<bool> laterThrows(parts.size(), false);
  for (size_t i = parts.size() - 1; i > 0; --i) {
    laterThrows[i - 1] = laterThrows[i] || codegen_.mayThrow().exprThrows(parts[i]);
  }
  std::vector<CodeGen::Operand> operands;
  for (size_t i = 0; i < parts.size(); ++i) {
    operands.push_back(emitConcatPart(parts[i], laterCannotAssign[i]));
    codegen_.throwLowerer().holdOperand(operands.back(), laterThrows[i]);
  }
  for (CodeGen::Operand &operand : operands) {
    codegen_.throwLowerer().takeOperand(operand);
  }

  return concatOperands(operands);
}

// A part of a concatenation. A number converted to a string goes to the
// runtime as it is, which writes its digits straight into the result instead
// of making a string for them first.
CodeGen::Operand StringLowerer::emitConcatPart(Expr *expr, bool canBorrow) {
  auto *conv = dyn_cast<CastExpr>(expr);
  const Type *from = conv && conv->isImplicit() ? conv->operand->type : nullptr;
  if (!from || !(from->isInteger() || from->isFloat())) {
    return codegen_.refCounts().emitOperand(expr, canBorrow);
  }
  llvm::Value *value = codegen_.exprLowerer().emitExpr(conv->operand);
  llvm::Value *bits = from->isFloat() ? codegen_.builder_.CreateBitCast(value, codegen_.builder_.getInt64Ty())
                                      : codegen_.builder_.CreateSExt(value, codegen_.builder_.getInt64Ty());
  llvm::Value *part = llvm::UndefValue::get(codegen_.strTy_);
  part = codegen_.builder_.CreateInsertValue(part, codegen_.builder_.CreateIntToPtr(bits, codegen_.builder_.getPtrTy()), 0);
  part = codegen_.builder_.CreateInsertValue(
      part, codegen_.builder_.getInt64(kNumberPart | (from->isFloat() ? kNumberDouble : kNumberInt)), 1);

  return {part, /*owned=*/false, Type::string()};
}

llvm::Value *StringLowerer::emitTemplate(TemplateExpr *tmpl) {
  size_t count = tmpl->exprs.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i) {
    laterCannotAssign[i - 2] = laterCannotAssign[i - 1] && AssignLowerer::cannotAssign(tmpl->exprs[i - 1]);
  }

  // `${x}` alone is just x as a string.
  if (count == 1 && tmpl->quasis[0].empty() && tmpl->quasis[1].empty()) {
    return codegen_.exprLowerer().emitExpr(tmpl->exprs[0]);
  }
  std::vector<bool> laterThrows(count, false);
  for (size_t i = count; i > 1; --i) {
    laterThrows[i - 2] = laterThrows[i - 1] || codegen_.mayThrow().exprThrows(tmpl->exprs[i - 1]);
  }
  std::vector<CodeGen::Operand> operands;
  for (size_t i = 0; i <= count; ++i) {
    if (!tmpl->quasis[i].empty()) {
      operands.push_back({stringLiteral(tmpl->quasis[i]), false, Type::string()});
    }
    if (i < count) {
      operands.push_back(emitConcatPart(tmpl->exprs[i], laterCannotAssign[i]));
      codegen_.throwLowerer().holdOperand(operands.back(), laterThrows[i]);
    }
  }
  for (CodeGen::Operand &operand : operands) {
    codegen_.throwLowerer().takeOperand(operand);
  }
  if (operands.empty()) {
    return stringLiteral(u"");
  }
  if (operands.size() == 1) { // a template without substitutions
    return operands[0].value;
  }

  return concatOperands(operands);
}

llvm::Value *StringLowerer::emitStringComparison(BinaryExpr *bin) {
  CodeGen::Operand lhs = codegen_.refCounts().emitOperand(bin->lhs, AssignLowerer::cannotAssign(bin->rhs));
  codegen_.throwLowerer().holdOperand(lhs, codegen_.mayThrow().exprThrows(bin->rhs));
  CodeGen::Operand rhs = codegen_.refCounts().emitOperand(bin->rhs, /*canBorrow=*/true);
  codegen_.throwLowerer().takeOperand(lhs);
  llvm::Value *args[] = {stringObj(lhs.value), stringMeta(lhs.value), stringObj(rhs.value),
                         stringMeta(rhs.value)};
  llvm::Value *zero = codegen_.builder_.getInt32(0);
  llvm::Value *result;
  switch (bin->op) {
  case BinaryOp::Eq:
    result = codegen_.equalityLowerer().emitValuesEqual(lhs.value, rhs.value, Type::string());
    break;
  case BinaryOp::Ne:
    result = codegen_.builder_.CreateNot(codegen_.equalityLowerer().emitValuesEqual(lhs.value, rhs.value, Type::string()));
    break;
  case BinaryOp::Lt:
    result = codegen_.builder_.CreateICmpSLT(codegen_.builder_.CreateCall(codegen_.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Le:
    result = codegen_.builder_.CreateICmpSLE(codegen_.builder_.CreateCall(codegen_.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Gt:
    result = codegen_.builder_.CreateICmpSGT(codegen_.builder_.CreateCall(codegen_.rt_.strCompare, args), zero);
    break;
  case BinaryOp::Ge:
    result = codegen_.builder_.CreateICmpSGE(codegen_.builder_.CreateCall(codegen_.rt_.strCompare, args), zero);
    break;
  default:
    llvm_unreachable("not a string comparison");
  }
  codegen_.refCounts().dropOperand(lhs);
  codegen_.refCounts().dropOperand(rhs);

  return result;
}

// `s[i]`: a string of one code unit; `undefined` out of range unless Sema
// proved the index in range.
llvm::Value *StringLowerer::emitStringIndex(IndexExpr *index) {
  CodeGen::Operand object = codegen_.refCounts().emitOperand(index->object, AssignLowerer::cannotAssign(index->index));
  codegen_.throwLowerer().holdOperand(object, codegen_.mayThrow().exprThrows(index->index));
  llvm::Value *position = codegen_.exprLowerer().emitExpr(index->index);
  codegen_.throwLowerer().takeOperand(object);
  auto read = [&] {
    return callReturningString(codegen_.rt_.strCharAt,
                               {stringObj(object.value), stringMeta(object.value), position});
  };
  llvm::Value *result;
  if (!index->mayBeOutOfRange) {
    result = read();
  } else {
    result = codegen_.arrayLowerer().emitCheckedRead(codegen_.builder_.CreateICmpULT(position, stringLength(object.value)),
                             index->type, read);
  }
  codegen_.refCounts().dropOperand(object);

  return result;
}

// A short literal is an inline string. A longer one lives in the executable
// in the same layout as a heap buffer (see runtime/core/string/string.c), one
// byte per code unit when all of them fit.
llvm::Constant *StringLowerer::stringLiteral(const std::u16string &text) {
  auto found = literals_.find(text);
  if (found != literals_.end()) {
    return found->second;
  }

  bool twoByte = llvm::any_of(text, [](char16_t c) { return c > 0xFF; });
  bool ascii = llvm::all_of(text, [](char16_t c) { return c < 0x80; });
  llvm::Constant *value;
  if (text.empty()) {
    value = llvm::ConstantStruct::get(codegen_.strTy_, {llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()),
                                               codegen_.builder_.getInt64(kStrAscii)});
  } else if (!twoByte && text.size() <= kInlineMax) {
    // The first code unit goes to the lowest address, zeros after the last.
    bool little = codegen_.module_->getDataLayout().isLittleEndian();
    uint64_t bits = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      bits |= uint64_t(static_cast<uint8_t>(text[i])) << (8 * (little ? i : 7 - i));
    }
    uint64_t meta = text.size() | kStrRope | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(
        codegen_.strTy_, {llvm::ConstantExpr::getIntToPtr(codegen_.builder_.getInt64(bits), codegen_.builder_.getPtrTy()),
                 codegen_.builder_.getInt64(meta)});
  } else {
    llvm::Constant *data;
    if (twoByte) {
      std::vector<uint16_t> units(text.begin(), text.end());
      data = llvm::ConstantDataArray::get(codegen_.ctx_, units);
    } else {
      std::vector<uint8_t> units;
      for (char16_t c : text) {
        units.push_back(static_cast<uint8_t>(c));
      }
      data = llvm::ConstantDataArray::get(codegen_.ctx_, units);
    }
    llvm::Type *i32 = codegen_.builder_.getInt32Ty();
    auto *layout =
        llvm::StructType::get(codegen_.ctx_, {codegen_.builder_.getInt64Ty(), i32, i32, i32, i32, data->getType()});
    auto length = static_cast<uint32_t>(text.size());
    llvm::Constant *init = llvm::ConstantStruct::get(
        layout, {codegen_.builder_.getInt64(kLiteralRefCount),
                 codegen_.builder_.getInt32(twoByte ? kKindBuffer2 : kKindBuffer1), codegen_.builder_.getInt32(0),
                 codegen_.builder_.getInt32(length), codegen_.builder_.getInt32(length), data});
    auto *global = new llvm::GlobalVariable(*codegen_.module_, layout, /*isConstant=*/true,
                                            llvm::GlobalValue::PrivateLinkage, init, ".str");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    global->setAlignment(llvm::Align(8));
    uint64_t meta = length | (twoByte ? kStrTwoByte : 0) | (ascii ? kStrAscii : 0);
    value = llvm::ConstantStruct::get(codegen_.strTy_, {global, codegen_.builder_.getInt64(meta)});
  }
  literals_.emplace(text, value);

  return value;
}

} // namespace tz
