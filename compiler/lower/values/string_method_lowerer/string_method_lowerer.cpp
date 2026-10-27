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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

StringMethodLowerer::StringMethodLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *StringMethodLowerer::emitStringMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  if (call->builtin == Builtin::StrToString) {
    return lowering_.exprLowerer().emitExpr(member->object);
  }
  if (call->builtin == Builtin::StrConcat) {
    std::vector<Expr *> parts{member->object};
    parts.insert(parts.end(), call->args.begin(), call->args.end());
    std::vector<Lowering::Operand> operands;
    for (size_t i = 0; i < parts.size(); ++i) {
      bool later = std::all_of(parts.begin() + i + 1, parts.end(), AssignLowerer::cannotAssign);
      operands.push_back(lowering_.stringLowerer().emitConcatPart(parts[i], later));
    }
    return lowering_.stringLowerer().concatOperands(operands);
  }

  Lowering::Operand receiver = lowering_.refCounts().emitOperand(
      member->object, llvm::all_of(call->args, AssignLowerer::cannotAssign));
  std::vector<Lowering::Operand> strings;
  std::vector<llvm::Value *> numbers;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    if (arg->type->isString()) {
      bool later =
          std::all_of(call->args.begin() + i + 1, call->args.end(), AssignLowerer::cannotAssign);
      strings.push_back(lowering_.refCounts().emitOperand(arg, later));
    } else {
      numbers.push_back(lowering_.builder_.CreateSExt(lowering_.exprLowerer().emitExpr(arg),
                                                      lowering_.builder_.getInt64Ty()));
    }
  }
  auto optional = [&](size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < numbers.size()) {
      return {numbers[i], lowering_.builder_.getInt32(1)};
    }

    return {lowering_.builder_.getInt64(0), lowering_.builder_.getInt32(0)};
  };
  auto optionalString = [&](size_t i) -> std::array<llvm::Value *, 3> {
    if (i < strings.size()) {
      return {lowering_.stringLowerer().stringObj(strings[i].value),
              lowering_.stringLowerer().stringMeta(strings[i].value),
              lowering_.builder_.getInt32(1)};
    }

    return {llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
            lowering_.builder_.getInt64(0), lowering_.builder_.getInt32(0)};
  };

  llvm::Value *obj = lowering_.stringLowerer().stringObj(receiver.value);
  llvm::Value *meta = lowering_.stringLowerer().stringMeta(receiver.value);
  llvm::Value *result = nullptr;
  bool failable = false;
  switch (call->builtin) {
  case Builtin::StrCharCodeAt: {
    llvm::Value *position = numbers[0];
    auto read = [&]() -> llvm::Value * {
      return lowering_.builder_.CreateSExt(
          lowering_.builder_.CreateCall(lowering_.rt_.strCharCodeAt, {obj, meta, position}),
          lowering_.builder_.getInt64Ty());
    };
    result = call->mayBeOutOfRange
                 ? lowering_.arrayLowerer().emitCheckedRead(
                       lowering_.builder_.CreateICmpULT(
                           position, lowering_.stringLowerer().stringLength(receiver.value)),
                       call->type, read)
                 : read();
    break;
  }
  case Builtin::StrAt: {
    llvm::Value *length = lowering_.stringLowerer().stringLength(receiver.value);
    llvm::Value *position = numbers[0];
    position = lowering_.builder_.CreateSelect(
        lowering_.builder_.CreateICmpSLT(position, lowering_.builder_.getInt64(0)),
        lowering_.builder_.CreateAdd(position, length), position);
    auto read = [&] {
      return lowering_.stringLowerer().callReturningString(lowering_.rt_.strCharAt,
                                                           {obj, meta, position});
    };
    result = call->mayBeOutOfRange
                 ? lowering_.arrayLowerer().emitCheckedRead(
                       lowering_.builder_.CreateICmpULT(position, length), call->type, read)
                 : read();
    break;
  }
  case Builtin::StrSlice:
  case Builtin::StrSubstring: {
    llvm::Value *start = numbers.empty() ? lowering_.builder_.getInt64(0) : numbers[0];
    auto [end, hasEnd] = optional(1);
    result = lowering_.stringLowerer().callReturningString(
        call->builtin == Builtin::StrSlice ? lowering_.rt_.strSlice : lowering_.rt_.strSubstring,
        {obj, meta, start, end, hasEnd});
    break;
  }
  case Builtin::StrIndexOf:
  case Builtin::StrIncludes:
  case Builtin::StrStartsWith:
  case Builtin::StrEndsWith: {
    llvm::FunctionCallee fn = call->builtin == Builtin::StrIndexOf    ? lowering_.rt_.strIndexOf
                              : call->builtin == Builtin::StrIncludes ? lowering_.rt_.strIncludes
                              : call->builtin == Builtin::StrStartsWith
                                  ? lowering_.rt_.strStartsWith
                                  : lowering_.rt_.strEndsWith;
    llvm::Value *search = strings[0].value;
    auto [position, hasPosition] = optional(0);
    result = lowering_.builder_.CreateCall(
        fn, {obj, meta, lowering_.stringLowerer().stringObj(search),
             lowering_.stringLowerer().stringMeta(search), position, hasPosition});
    result = call->builtin != Builtin::StrIndexOf
                 ? lowering_.builder_.CreateICmpNE(result, lowering_.builder_.getInt32(0))
                 : lowering_.builder_.CreateSExt(result, lowering_.builder_.getInt64Ty());
    break;
  }
  case Builtin::StrRepeat:
    result = lowering_.stringLowerer().callReturningString(lowering_.rt_.strRepeat,
                                                           {obj, meta, numbers[0]});
    failable = true;
    break;
  case Builtin::StrCharAt:
    result = lowering_.stringLowerer().callReturningString(
        lowering_.rt_.strCharAtOrEmpty,
        {obj, meta, numbers.empty() ? lowering_.builder_.getInt64(0) : numbers[0]});
    break;
  case Builtin::StrCodePointAt: {
    llvm::Value *position = numbers[0];
    auto read = [&]() -> llvm::Value * {
      return lowering_.builder_.CreateSExt(
          lowering_.builder_.CreateCall(lowering_.rt_.strCodePoint, {obj, meta, position}),
          lowering_.builder_.getInt64Ty());
    };
    result = call->mayBeOutOfRange
                 ? lowering_.arrayLowerer().emitCheckedRead(
                       lowering_.builder_.CreateICmpULT(
                           position, lowering_.stringLowerer().stringLength(receiver.value)),
                       call->type, read)
                 : read();
    break;
  }
  case Builtin::StrLastIndexOf: {
    llvm::Value *search = strings[0].value;
    auto [position, hasPosition] = optional(0);
    result = lowering_.builder_.CreateCall(lowering_.rt_.strLastIndexOf,
                                           {obj, meta, lowering_.stringLowerer().stringObj(search),
                                            lowering_.stringLowerer().stringMeta(search), position,
                                            hasPosition});
    result = lowering_.builder_.CreateSExt(result, lowering_.builder_.getInt64Ty());
    break;
  }
  case Builtin::StrSplit: {
    auto [sepObj, sepMeta, hasSep] = optionalString(0);
    auto [limit, hasLimit] = optional(0);
    result =
        lowering_.builder_.CreateCall(lowering_.rt_.strSplit,
                                      {lowering_.typeDescriptors().typeDescriptor(Type::string()),
                                       obj, meta, sepObj, sepMeta, hasSep, limit, hasLimit},
                                      "split");
    break;
  }
  case Builtin::StrTrim:
  case Builtin::StrTrimStart:
  case Builtin::StrTrimEnd: {
    int ends = call->builtin == Builtin::StrTrimStart ? 1
               : call->builtin == Builtin::StrTrimEnd ? 2
                                                      : 3;
    result = lowering_.stringLowerer().callReturningString(
        lowering_.rt_.strTrim, {obj, meta, lowering_.builder_.getInt32(ends)});
    break;
  }
  case Builtin::StrPadStart:
  case Builtin::StrPadEnd: {
    auto [fillObj, fillMeta, hasFill] = optionalString(0);
    result = lowering_.stringLowerer().callReturningString(
        lowering_.rt_.strPad, {obj, meta, numbers[0], fillObj, fillMeta, hasFill,
                               lowering_.builder_.getInt32(call->builtin == Builtin::StrPadStart)});
    failable = true;
    break;
  }
  case Builtin::StrToUpperCase:
  case Builtin::StrToLowerCase:
    result = lowering_.stringLowerer().callReturningString(call->builtin == Builtin::StrToUpperCase
                                                               ? lowering_.rt_.strToUpper
                                                               : lowering_.rt_.strToLower,
                                                           {obj, meta});
    break;
  case Builtin::StrReplace:
  case Builtin::StrReplaceAll: {
    llvm::Value *pattern = strings[0].value, *replacement = strings[1].value;
    result = lowering_.stringLowerer().callReturningString(
        lowering_.rt_.strReplace,
        {obj, meta, lowering_.stringLowerer().stringObj(pattern),
         lowering_.stringLowerer().stringMeta(pattern),
         lowering_.stringLowerer().stringObj(replacement),
         lowering_.stringLowerer().stringMeta(replacement),
         lowering_.builder_.getInt32(call->builtin == Builtin::StrReplaceAll)});
    break;
  }
  default:
    llvm_unreachable("not a string method");
  }

  lowering_.refCounts().dropOperand(receiver);
  for (const Lowering::Operand &str : strings) {
    lowering_.refCounts().dropOperand(str);
  }

  return result;
}

}
