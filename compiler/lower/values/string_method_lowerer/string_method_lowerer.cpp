#include "compiler/lower/values/string_method_lowerer/string_method_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::cast;

namespace tz {

StringMethodLowerer::StringMethodLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *StringMethodLowerer::emitStringMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  if (call->builtin() == Builtin::StrToString) {
    return this->lowering.exprLowerer().emitExpr(member->object);
  }
  if (call->builtin() == Builtin::StrConcat) {
    std::vector<Expr *> parts{member->object};
    parts.insert(parts.end(), call->args.begin(), call->args.end());
    std::vector<Lowering::Operand> operands;
    for (size_t i = 0; i < parts.size(); ++i) {
      bool later = std::all_of(parts.begin() + i + 1, parts.end(), AssignLowerer::cannotAssign);
      operands.push_back(this->lowering.stringLowerer().emitConcatPart(parts[i], later));
    }
    return this->lowering.stringLowerer().concatOperands(operands);
  }

  Lowering::Operand receiver = this->lowering.refCounts().emitOperand(
      member->object, llvm::all_of(call->args, AssignLowerer::cannotAssign));
  std::vector<Lowering::Operand> strings;
  std::vector<llvm::Value *> numbers;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    if (arg->type->isString()) {
      bool later =
          std::all_of(call->args.begin() + i + 1, call->args.end(), AssignLowerer::cannotAssign);
      strings.push_back(this->lowering.refCounts().emitOperand(arg, later));
    } else {
      numbers.push_back(this->lowering.builder_.CreateSExt(
          this->lowering.exprLowerer().emitExpr(arg), this->lowering.builder_.getInt64Ty()));
    }
  }
  auto optional = [&](size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < numbers.size()) {
      return {numbers[i], this->lowering.builder_.getInt32(1)};
    }

    return {this->lowering.builder_.getInt64(0), this->lowering.builder_.getInt32(0)};
  };
  auto optionalString = [&](size_t i) -> std::array<llvm::Value *, 3> {
    if (i < strings.size()) {
      return {this->lowering.stringLowerer().stringObj(strings[i].value),
              this->lowering.stringLowerer().stringMeta(strings[i].value),
              this->lowering.builder_.getInt32(1)};
    }

    return {llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()),
            this->lowering.builder_.getInt64(0), this->lowering.builder_.getInt32(0)};
  };

  llvm::Value *obj = this->lowering.stringLowerer().stringObj(receiver.value);
  llvm::Value *meta = this->lowering.stringLowerer().stringMeta(receiver.value);
  llvm::Value *result = nullptr;
  bool failable = false;
  switch (call->builtin()) {
    case Builtin::StrCharCodeAt: {
      llvm::Value *position = numbers[0];
      auto read = [&]() -> llvm::Value * {
        return this->lowering.builder_.CreateSExt(
            this->lowering.builder_.CreateCall(this->lowering.rt_.strCharCodeAt,
                                               {obj, meta, position}),
            this->lowering.builder_.getInt64Ty());
      };
      result = call->mayBeOutOfRange
                   ? this->lowering.arrayLowerer().emitCheckedRead(
                         this->lowering.builder_.CreateICmpULT(
                             position, this->lowering.stringLowerer().stringLength(receiver.value)),
                         call->type, read)
                   : read();
      break;
    }
    case Builtin::StrAt: {
      llvm::Value *length = this->lowering.stringLowerer().stringLength(receiver.value);
      llvm::Value *position = numbers[0];
      position = this->lowering.builder_.CreateSelect(
          this->lowering.builder_.CreateICmpSLT(position, this->lowering.builder_.getInt64(0)),
          this->lowering.builder_.CreateAdd(position, length), position);
      auto read = [&] {
        return this->lowering.stringLowerer().callReturningString(this->lowering.rt_.strCharAt,
                                                                  {obj, meta, position});
      };
      result = call->mayBeOutOfRange
                   ? this->lowering.arrayLowerer().emitCheckedRead(
                         this->lowering.builder_.CreateICmpULT(position, length), call->type, read)
                   : read();
      break;
    }
    case Builtin::StrSlice:
    case Builtin::StrSubstring: {
      llvm::Value *start = numbers.empty() ? this->lowering.builder_.getInt64(0) : numbers[0];
      auto [end, hasEnd] = optional(1);
      result = this->lowering.stringLowerer().callReturningString(
          call->builtin() == Builtin::StrSlice ? this->lowering.rt_.strSlice
                                               : this->lowering.rt_.strSubstring,
          {obj, meta, start, end, hasEnd});
      break;
    }
    case Builtin::StrIndexOf:
    case Builtin::StrIncludes:
    case Builtin::StrStartsWith:
    case Builtin::StrEndsWith: {
      llvm::FunctionCallee fn =
          call->builtin() == Builtin::StrIndexOf      ? this->lowering.rt_.strIndexOf
          : call->builtin() == Builtin::StrIncludes   ? this->lowering.rt_.strIncludes
          : call->builtin() == Builtin::StrStartsWith ? this->lowering.rt_.strStartsWith
                                                      : this->lowering.rt_.strEndsWith;
      llvm::Value *search = strings[0].value;
      auto [position, hasPosition] = optional(0);
      result = this->lowering.builder_.CreateCall(
          fn, {obj, meta, this->lowering.stringLowerer().stringObj(search),
               this->lowering.stringLowerer().stringMeta(search), position, hasPosition});
      result =
          call->builtin() != Builtin::StrIndexOf
              ? this->lowering.builder_.CreateICmpNE(result, this->lowering.builder_.getInt32(0))
              : this->lowering.builder_.CreateSExt(result, this->lowering.builder_.getInt64Ty());
      break;
    }
    case Builtin::StrRepeat:
      result = this->lowering.stringLowerer().callReturningString(this->lowering.rt_.strRepeat,
                                                                  {obj, meta, numbers[0]});
      failable = true;
      break;
    case Builtin::StrCharAt:
      result = this->lowering.stringLowerer().callReturningString(
          this->lowering.rt_.strCharAtOrEmpty,
          {obj, meta, numbers.empty() ? this->lowering.builder_.getInt64(0) : numbers[0]});
      break;
    case Builtin::StrCodePointAt: {
      llvm::Value *position = numbers[0];
      auto read = [&]() -> llvm::Value * {
        return this->lowering.builder_.CreateSExt(
            this->lowering.builder_.CreateCall(this->lowering.rt_.strCodePoint,
                                               {obj, meta, position}),
            this->lowering.builder_.getInt64Ty());
      };
      result = call->mayBeOutOfRange
                   ? this->lowering.arrayLowerer().emitCheckedRead(
                         this->lowering.builder_.CreateICmpULT(
                             position, this->lowering.stringLowerer().stringLength(receiver.value)),
                         call->type, read)
                   : read();
      break;
    }
    case Builtin::StrLastIndexOf: {
      llvm::Value *search = strings[0].value;
      auto [position, hasPosition] = optional(0);
      result = this->lowering.builder_.CreateCall(
          this->lowering.rt_.strLastIndexOf,
          {obj, meta, this->lowering.stringLowerer().stringObj(search),
           this->lowering.stringLowerer().stringMeta(search), position, hasPosition});
      result = this->lowering.builder_.CreateSExt(result, this->lowering.builder_.getInt64Ty());
      break;
    }
    case Builtin::StrSplit: {
      auto [sepObj, sepMeta, hasSep] = optionalString(0);
      auto [limit, hasLimit] = optional(0);
      result = this->lowering.builder_.CreateCall(
          this->lowering.rt_.strSplit,
          {this->lowering.typeDescriptors().typeDescriptor(Type::string()), obj, meta, sepObj,
           sepMeta, hasSep, limit, hasLimit},
          "split");
      break;
    }
    case Builtin::StrTrim:
    case Builtin::StrTrimStart:
    case Builtin::StrTrimEnd: {
      int ends = call->builtin() == Builtin::StrTrimStart ? 1
                 : call->builtin() == Builtin::StrTrimEnd ? 2
                                                          : 3;
      result = this->lowering.stringLowerer().callReturningString(
          this->lowering.rt_.strTrim, {obj, meta, this->lowering.builder_.getInt32(ends)});
      break;
    }
    case Builtin::StrPadStart:
    case Builtin::StrPadEnd: {
      auto [fillObj, fillMeta, hasFill] = optionalString(0);
      result = this->lowering.stringLowerer().callReturningString(
          this->lowering.rt_.strPad,
          {obj, meta, numbers[0], fillObj, fillMeta, hasFill,
           this->lowering.builder_.getInt32(call->builtin() == Builtin::StrPadStart)});
      failable = true;
      break;
    }
    case Builtin::StrToUpperCase:
    case Builtin::StrToLowerCase:
      result = this->lowering.stringLowerer().callReturningString(
          call->builtin() == Builtin::StrToUpperCase ? this->lowering.rt_.strToUpper
                                                     : this->lowering.rt_.strToLower,
          {obj, meta});
      break;
    case Builtin::StrReplace:
    case Builtin::StrReplaceAll: {
      llvm::Value *pattern = strings[0].value;
      llvm::Value *replacement = strings[1].value;
      result = this->lowering.stringLowerer().callReturningString(
          this->lowering.rt_.strReplace,
          {obj, meta, this->lowering.stringLowerer().stringObj(pattern),
           this->lowering.stringLowerer().stringMeta(pattern),
           this->lowering.stringLowerer().stringObj(replacement),
           this->lowering.stringLowerer().stringMeta(replacement),
           this->lowering.builder_.getInt32(call->builtin() == Builtin::StrReplaceAll)});
      break;
    }
    default:
      llvm_unreachable("not a string method");
  }

  this->lowering.refCounts().dropOperand(receiver);
  for (const Lowering::Operand &str : strings) {
    this->lowering.refCounts().dropOperand(str);
  }

  return result;
}

}
