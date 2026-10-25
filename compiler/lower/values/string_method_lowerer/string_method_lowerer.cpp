#include "compiler/lower/values/string_method_lowerer/string_method_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
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

StringMethodLowerer::StringMethodLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *StringMethodLowerer::emitStringMethod(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  if (call->builtin == Builtin::StrToString) {
    return codegen_.exprLowerer().emitExpr(member->object);
  }
  if (call->builtin == Builtin::StrConcat) {
    // `s.concat(a, b)` is `s + a + b`.
    std::vector<Expr *> parts{member->object};
    parts.insert(parts.end(), call->args.begin(), call->args.end());
    std::vector<CodeGen::Operand> operands;
    for (size_t i = 0; i < parts.size(); ++i) {
      bool later = std::all_of(parts.begin() + i + 1, parts.end(), AssignLowerer::cannotAssign);
      operands.push_back(codegen_.stringLowerer().emitConcatPart(parts[i], later));
    }
    return codegen_.stringLowerer().concatOperands(operands);
  }

  CodeGen::Operand receiver = codegen_.refCounts().emitOperand(member->object, llvm::all_of(call->args, AssignLowerer::cannotAssign));
  codegen_.throwLowerer().holdOperand(receiver, llvm::any_of(call->args, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));
  std::vector<CodeGen::Operand> strings;
  std::vector<llvm::Value *> numbers; // positions and counts, as i64
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    if (arg->type->isString()) {
      bool later = std::all_of(call->args.begin() + i + 1, call->args.end(), AssignLowerer::cannotAssign);
      strings.push_back(codegen_.refCounts().emitOperand(arg, later));
      codegen_.throwLowerer().holdOperand(strings.back(), std::any_of(call->args.begin() + i + 1, call->args.end(),
                                              [&](Expr *next) { return codegen_.mayThrow().exprThrows(next); }));
    } else {
      numbers.push_back(codegen_.builder_.CreateSExt(codegen_.exprLowerer().emitExpr(arg), codegen_.builder_.getInt64Ty()));
    }
  }
  codegen_.throwLowerer().takeOperand(receiver);
  for (CodeGen::Operand &str : strings) {
    codegen_.throwLowerer().takeOperand(str);
  }
  // An optional position: its value and whether it was given.
  auto optional = [&](size_t i) -> std::pair<llvm::Value *, llvm::Value *> {
    if (i < numbers.size()) {
      return {numbers[i], codegen_.builder_.getInt32(1)};
    }

    return {codegen_.builder_.getInt64(0), codegen_.builder_.getInt32(0)};
  };
  // An optional string argument: its parts and whether it was given.
  auto optionalString = [&](size_t i) -> std::array<llvm::Value *, 3> {
    if (i < strings.size()) {
      return {codegen_.stringLowerer().stringObj(strings[i].value), codegen_.stringLowerer().stringMeta(strings[i].value), codegen_.builder_.getInt32(1)};
    }

    return {llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()), codegen_.builder_.getInt64(0),
            codegen_.builder_.getInt32(0)};
  };

  llvm::Value *obj = codegen_.stringLowerer().stringObj(receiver.value);
  llvm::Value *meta = codegen_.stringLowerer().stringMeta(receiver.value);
  llvm::Value *result = nullptr;
  bool failable = false; // the runtime may throw
  switch (call->builtin) {
  case Builtin::StrCharCodeAt: {
    llvm::Value *position = numbers[0];
    if (!call->mayBeOutOfRange) {
      result = codegen_.builder_.CreateCall(codegen_.rt_.strCharCodeAt, {obj, meta, position});
      break;
    }
    // Out of range, JavaScript returns NaN.
    llvm::Value *inRange = codegen_.builder_.CreateICmpULT(position, codegen_.stringLowerer().stringLength(receiver.value));
    llvm::BasicBlock *readBlock = codegen_.functionFrame().newBlock("code.read");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("code.end");
    llvm::BasicBlock *before = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateCondBr(inRange, readBlock, endBlock);
    codegen_.functionFrame().switchTo(readBlock);
    llvm::Value *code = codegen_.builder_.CreateSIToFP(
        codegen_.builder_.CreateCall(codegen_.rt_.strCharCodeAt, {obj, meta, position}), codegen_.builder_.getDoubleTy());
    llvm::BasicBlock *readEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.builder_.getDoubleTy(), 2);
    phi->addIncoming(llvm::ConstantFP::getNaN(codegen_.builder_.getDoubleTy()), before);
    phi->addIncoming(code, readEnd);
    result = phi;
    break;
  }
  case Builtin::StrAt: {
    // A negative position counts from the end.
    llvm::Value *length = codegen_.stringLowerer().stringLength(receiver.value);
    llvm::Value *position = numbers[0];
    position = codegen_.builder_.CreateSelect(codegen_.builder_.CreateICmpSLT(position, codegen_.builder_.getInt64(0)),
                                     codegen_.builder_.CreateAdd(position, length), position);
    auto read = [&] { return codegen_.stringLowerer().callReturningString(codegen_.rt_.strCharAt, {obj, meta, position}); };
    result = call->mayBeOutOfRange
                 ? codegen_.arrayLowerer().emitCheckedRead(codegen_.builder_.CreateICmpULT(position, length), call->type, read)
                 : read();
    break;
  }
  case Builtin::StrSlice:
  case Builtin::StrSubstring: {
    llvm::Value *start = numbers.empty() ? codegen_.builder_.getInt64(0) : numbers[0];
    auto [end, hasEnd] = optional(1);
    result =
        codegen_.stringLowerer().callReturningString(call->builtin == Builtin::StrSlice ? codegen_.rt_.strSlice : codegen_.rt_.strSubstring,
                            {obj, meta, start, end, hasEnd});
    break;
  }
  case Builtin::StrIndexOf:
  case Builtin::StrIncludes:
  case Builtin::StrStartsWith:
  case Builtin::StrEndsWith: {
    llvm::FunctionCallee fn = call->builtin == Builtin::StrIndexOf      ? codegen_.rt_.strIndexOf
                              : call->builtin == Builtin::StrIncludes   ? codegen_.rt_.strIncludes
                              : call->builtin == Builtin::StrStartsWith ? codegen_.rt_.strStartsWith
                                                                        : codegen_.rt_.strEndsWith;
    llvm::Value *search = strings[0].value;
    auto [position, hasPosition] = optional(0);
    result = codegen_.builder_.CreateCall(
        fn, {obj, meta, codegen_.stringLowerer().stringObj(search), codegen_.stringLowerer().stringMeta(search), position, hasPosition});
    if (call->builtin != Builtin::StrIndexOf) {
      result = codegen_.builder_.CreateICmpNE(result, codegen_.builder_.getInt32(0));
    }
    break;
  }
  case Builtin::StrRepeat:
    result = codegen_.stringLowerer().callReturningString(codegen_.rt_.strRepeat, {obj, meta, numbers[0]});
    failable = true; // a negative count
    break;
  case Builtin::StrCharAt: // "" out of range
    result = codegen_.stringLowerer().callReturningString(codegen_.rt_.strCharAtOrEmpty,
                                 {obj, meta, numbers.empty() ? codegen_.builder_.getInt64(0) : numbers[0]});
    break;
  case Builtin::StrCodePointAt: {
    llvm::Value *position = numbers[0];
    auto read = [&]() -> llvm::Value * {
      return codegen_.builder_.CreateCall(codegen_.rt_.strCodePoint, {obj, meta, position});
    };
    result = call->mayBeOutOfRange
                 ? codegen_.arrayLowerer().emitCheckedRead(codegen_.builder_.CreateICmpULT(position, codegen_.stringLowerer().stringLength(receiver.value)),
                                   call->type, read)
                 : read();
    break;
  }
  case Builtin::StrLastIndexOf: {
    llvm::Value *search = strings[0].value;
    auto [position, hasPosition] = optional(0);
    result = codegen_.builder_.CreateCall(codegen_.rt_.strLastIndexOf, {obj, meta, codegen_.stringLowerer().stringObj(search),
                                                      codegen_.stringLowerer().stringMeta(search), position, hasPosition});
    break;
  }
  case Builtin::StrSplit: {
    auto [sepObj, sepMeta, hasSep] = optionalString(0);
    auto [limit, hasLimit] = optional(0);
    result = codegen_.builder_.CreateCall(
        codegen_.rt_.strSplit,
        {codegen_.typeDescriptors().typeDescriptor(Type::string()), obj, meta, sepObj, sepMeta, hasSep, limit, hasLimit},
        "split");
    break;
  }
  case Builtin::StrTrim:
  case Builtin::StrTrimStart:
  case Builtin::StrTrimEnd: {
    int ends = call->builtin == Builtin::StrTrimStart ? 1
               : call->builtin == Builtin::StrTrimEnd ? 2
                                                      : 3;
    result = codegen_.stringLowerer().callReturningString(codegen_.rt_.strTrim, {obj, meta, codegen_.builder_.getInt32(ends)});
    break;
  }
  case Builtin::StrPadStart:
  case Builtin::StrPadEnd: {
    auto [fillObj, fillMeta, hasFill] = optionalString(0);
    result =
        codegen_.stringLowerer().callReturningString(codegen_.rt_.strPad, {obj, meta, numbers[0], fillObj, fillMeta, hasFill,
                                         codegen_.builder_.getInt32(call->builtin == Builtin::StrPadStart)});
    failable = true; // a string too long
    break;
  }
  case Builtin::StrToUpperCase:
  case Builtin::StrToLowerCase:
    result = codegen_.stringLowerer().callReturningString(
        call->builtin == Builtin::StrToUpperCase ? codegen_.rt_.strToUpper : codegen_.rt_.strToLower, {obj, meta});
    break;
  case Builtin::StrReplace:
  case Builtin::StrReplaceAll: {
    llvm::Value *pattern = strings[0].value, *replacement = strings[1].value;
    result = codegen_.stringLowerer().callReturningString(codegen_.rt_.strReplace,
                                 {obj, meta, codegen_.stringLowerer().stringObj(pattern), codegen_.stringLowerer().stringMeta(pattern),
                                  codegen_.stringLowerer().stringObj(replacement), codegen_.stringLowerer().stringMeta(replacement),
                                  codegen_.builder_.getInt32(call->builtin == Builtin::StrReplaceAll)});
    break;
  }
  default:
    llvm_unreachable("not a string method");
  }

  codegen_.refCounts().dropOperand(receiver);
  for (const CodeGen::Operand &str : strings) {
    codegen_.refCounts().dropOperand(str);
  }
  codegen_.emitThrowCheckIf(failable);

  return result;
}

} // namespace tz
