#include "compiler/library/console/console_lowerer/console_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
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

ConsoleLowerer::ConsoleLowerer(Lowering &lowering) : lowering_(lowering) {}

// console.log(a, b) prints its arguments separated by spaces, then a newline.
// Strings print as they are; arrays and optionals the way Node.js shows them.
// As in JavaScript, all arguments are evaluated before anything is printed:
// `console.log(a.pop(), a)` shows the array without the popped element.
// console.error prints the same way to stderr (see tz_err_begin in
// runtime/core/output/output.c).
void ConsoleLowerer::emitConsoleLog(CallExpr *call) {
  size_t count = call->args.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i) {
    laterCannotAssign[i - 2] =
        laterCannotAssign[i - 1] && AssignLowerer::cannotAssign(call->args[i - 1]);
  }
  std::vector<bool> laterThrows(count, false);
  for (size_t i = count; i > 1; --i) {
    laterThrows[i - 2] = laterThrows[i - 1] || lowering_.mayThrow().exprThrows(call->args[i - 1]);
  }
  std::vector<Lowering::Operand> values;
  for (size_t i = 0; i < count; ++i) {
    if (call->args[i]->type->isUndefined()) {
      lowering_.stmtLowerer().emitDiscarded(call->args[i]); // `void f()` still runs f
      values.push_back({nullptr, false, call->args[i]->type});
    } else {
      values.push_back(lowering_.refCounts().emitOperand(call->args[i], laterCannotAssign[i]));
      lowering_.throwLowerer().holdOperand(values.back(), laterThrows[i]);
    }
  }
  for (Lowering::Operand &value : values) {
    lowering_.throwLowerer().takeOperand(value);
  }

  bool toStderr = call->builtin == Builtin::ConsoleError;
  if (toStderr) {
    lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
        "tz_err_begin", lowering_.builder_.getVoidTy(), {}));
  }
  auto print = [&](llvm::Value *value, const Type *type) {
    if (type->isUndefined() || type->isNever()) {
      llvm::Value *text = lowering_.stringLowerer().stringLiteral(u"undefined");
      lowering_.builder_.CreateCall(
          lowering_.rt_.printString,
          {lowering_.stringLowerer().stringObj(text), lowering_.stringLowerer().stringMeta(text)});
      return;
    }
    switch (type->kind()) {
    case Type::Kind::String:
      lowering_.builder_.CreateCall(lowering_.rt_.printString,
                                    {lowering_.stringLowerer().stringObj(value),
                                     lowering_.stringLowerer().stringMeta(value)});
      break;
    case Type::Kind::Int:
      lowering_.builder_.CreateCall(lowering_.rt_.printI32, {value});
      break;
    case Type::Kind::Long:
      lowering_.builder_.CreateCall(lowering_.rt_.printI64, {value});
      break;
    case Type::Kind::Double:
      lowering_.builder_.CreateCall(lowering_.rt_.printF64, {value});
      break;
    case Type::Kind::Bool:
      lowering_.builder_.CreateCall(
          lowering_.rt_.printBool,
          {lowering_.builder_.CreateZExt(value, lowering_.builder_.getInt32Ty())});
      break;
    case Type::Kind::Decimal: {
      llvm::Type *i64 = lowering_.builder_.getInt64Ty();
      lowering_.builder_.CreateCall(
          lowering_.decimalLowerer().decimalFunction("tz_print_decimal",
                                                     lowering_.builder_.getVoidTy(), {i64, i64}),
          {lowering_.decimalLowerer().decimalCoef(value),
           lowering_.decimalLowerer().decimalMeta(value)});
      break;
    }
    case Type::Kind::Array:
    case Type::Kind::Optional:
    case Type::Kind::Function:
    case Type::Kind::Object:
    case Type::Kind::Result:
    case Type::Kind::Iterator:
    case Type::Kind::Json:
    case Type::Kind::Dict:
    case Type::Kind::Literal:
    case Type::Kind::Union:
    case Type::Kind::Promise:
    case Type::Kind::Unknown:
    case Type::Kind::ClassRef:
      lowering_.builder_.CreateCall(lowering_.rt_.printValue,
                                    {lowering_.typeDescriptors().typeDescriptor(type),
                                     lowering_.functionFrame().spill(value, "print.value")});
      break;
    default:
      llvm_unreachable("Checker rejects other argument types");
    }
  };
  // `console.log(...xs)` prints the elements as arguments of their own: a
  // space goes before each value but the first printed.
  llvm::Value *first = nullptr;
  if (call->hasSpread()) {
    first =
        lowering_.functionFrame().createStackSlot(lowering_.builder_.getInt1Ty(), "print.first");
    lowering_.builder_.CreateStore(lowering_.builder_.getTrue(), first);
  }
  auto separate = [&](size_t i) {
    if (!first) {
      if (i > 0) {
        lowering_.builder_.CreateCall(lowering_.rt_.printChar, {lowering_.builder_.getInt32(' ')});
      }
      return;
    }
    llvm::BasicBlock *spaceBlock = lowering_.functionFrame().newBlock("print.space");
    llvm::BasicBlock *valueBlock = lowering_.functionFrame().newBlock("print.value");
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateLoad(lowering_.builder_.getInt1Ty(), first), valueBlock,
        spaceBlock);
    lowering_.functionFrame().switchTo(spaceBlock);
    lowering_.builder_.CreateCall(lowering_.rt_.printChar, {lowering_.builder_.getInt32(' ')});
    lowering_.builder_.CreateBr(valueBlock);
    lowering_.functionFrame().switchTo(valueBlock);
    lowering_.builder_.CreateStore(lowering_.builder_.getFalse(), first);
  };
  for (size_t i = 0; i < count; ++i) {
    const Lowering::Operand &value = values[i];
    if (call->spreads(i) && value.type->isArray()) {
      // Each element, in a loop.
      const Type *element = value.type->inner();
      llvm::Value *length = lowering_.arrayLowerer().arrayLength(value.value);
      llvm::Value *indexSlot =
          lowering_.functionFrame().createStackSlot(lowering_.builder_.getInt64Ty(), "print.index");
      lowering_.builder_.CreateStore(lowering_.builder_.getInt64(0), indexSlot);
      llvm::BasicBlock *condBlock = lowering_.functionFrame().newBlock("print.cond");
      llvm::BasicBlock *bodyBlock = lowering_.functionFrame().newBlock("print.body");
      llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("print.end");
      lowering_.builder_.CreateBr(condBlock);
      lowering_.functionFrame().switchTo(condBlock);
      llvm::Value *index =
          lowering_.builder_.CreateLoad(lowering_.builder_.getInt64Ty(), indexSlot);
      lowering_.builder_.CreateCondBr(lowering_.builder_.CreateICmpULT(index, length), bodyBlock,
                                      endBlock);
      lowering_.functionFrame().switchTo(bodyBlock);
      separate(i);
      llvm::Value *at = lowering_.builder_.CreateLoad(lowering_.builder_.getInt64Ty(), indexSlot);
      llvm::Value *item = lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(element),
          lowering_.arrayLowerer().elementAddress(lowering_.arrayLowerer().arrayData(value.value),
                                                  element, at),
          "item");
      print(item, element);
      lowering_.builder_.CreateStore(
          lowering_.builder_.CreateAdd(at, lowering_.builder_.getInt64(1)), indexSlot);
      lowering_.builder_.CreateBr(condBlock);
      lowering_.functionFrame().switchTo(endBlock);
      lowering_.refCounts().dropOperand(value);
      continue;
    }
    separate(i);
    print(value.value, value.type);
    lowering_.refCounts().dropOperand(value);
  }
  lowering_.builder_.CreateCall(lowering_.rt_.printChar, {lowering_.builder_.getInt32('\n')});
  if (toStderr) {
    lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
        "tz_err_end", lowering_.builder_.getVoidTy(), {}));
  }
}

} // namespace tz
