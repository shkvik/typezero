#include "compiler/library/console/console_lowerer/console_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
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

ConsoleLowerer::ConsoleLowerer(CodeGen &codegen) : codegen_(codegen) {}

// console.log(a, b) prints its arguments separated by spaces, then a newline.
// Strings print as they are; arrays and optionals the way Node.js shows them.
// As in JavaScript, all arguments are evaluated before anything is printed:
// `console.log(a.pop(), a)` shows the array without the popped element.
// console.error prints the same way to stderr (see tz_err_begin in
// runtime/core/tzrt/tzrt.c).
void ConsoleLowerer::emitConsoleLog(CallExpr *call) {
  size_t count = call->args.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i) {
    laterCannotAssign[i - 2] = laterCannotAssign[i - 1] && AssignLowerer::cannotAssign(call->args[i - 1]);
  }
  std::vector<bool> laterThrows(count, false);
  for (size_t i = count; i > 1; --i) {
    laterThrows[i - 2] = laterThrows[i - 1] || codegen_.mayThrow().exprThrows(call->args[i - 1]);
  }
  std::vector<CodeGen::Operand> values;
  for (size_t i = 0; i < count; ++i) {
    if (call->args[i]->type->isUndefined()) {
      codegen_.stmtLowerer().emitDiscarded(call->args[i]); // `void f()` still runs f
      values.push_back({nullptr, false, call->args[i]->type});
    } else {
      values.push_back(codegen_.refCounts().emitOperand(call->args[i], laterCannotAssign[i]));
      codegen_.throwLowerer().holdOperand(values.back(), laterThrows[i]);
    }
  }
  for (CodeGen::Operand &value : values) {
    codegen_.throwLowerer().takeOperand(value);
  }

  bool toStderr = call->builtin == Builtin::ConsoleError;
  if (toStderr) {
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_err_begin", codegen_.builder_.getVoidTy(), {}));
  }
  auto print = [&](llvm::Value *value, const Type *type) {
    if (type->isUndefined() || type->isNever()) {
      llvm::Value *text = codegen_.stringLowerer().stringLiteral(u"undefined");
      codegen_.builder_.CreateCall(codegen_.rt_.printString, {codegen_.stringLowerer().stringObj(text), codegen_.stringLowerer().stringMeta(text)});
      return;
    }
    switch (type->kind()) {
    case Type::Kind::String:
      codegen_.builder_.CreateCall(codegen_.rt_.printString, {codegen_.stringLowerer().stringObj(value), codegen_.stringLowerer().stringMeta(value)});
      break;
    case Type::Kind::Int:
      codegen_.builder_.CreateCall(codegen_.rt_.printI32, {value});
      break;
    case Type::Kind::Long:
      codegen_.builder_.CreateCall(codegen_.rt_.printI64, {value});
      break;
    case Type::Kind::Double:
      codegen_.builder_.CreateCall(codegen_.rt_.printF64, {value});
      break;
    case Type::Kind::Bool:
      codegen_.builder_.CreateCall(codegen_.rt_.printBool, {codegen_.builder_.CreateZExt(value, codegen_.builder_.getInt32Ty())});
      break;
    case Type::Kind::Decimal: {
      llvm::Type *i64 = codegen_.builder_.getInt64Ty();
      codegen_.builder_.CreateCall(codegen_.decimalLowerer().decimalFunction("tz_print_decimal", codegen_.builder_.getVoidTy(), {i64, i64}),
                          {codegen_.decimalLowerer().decimalCoef(value), codegen_.decimalLowerer().decimalMeta(value)});
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
      codegen_.builder_.CreateCall(codegen_.rt_.printValue, {codegen_.typeDescriptors().typeDescriptor(type), codegen_.functionFrame().spill(value, "print.value")});
      break;
    default:
      llvm_unreachable("Sema rejects other argument types");
    }
  };
  // `console.log(...xs)` prints the elements as arguments of their own: a
  // space goes before each value but the first printed.
  llvm::Value *first = nullptr;
  if (call->hasSpread()) {
    first = codegen_.functionFrame().createStackSlot(codegen_.builder_.getInt1Ty(), "print.first");
    codegen_.builder_.CreateStore(codegen_.builder_.getTrue(), first);
  }
  auto separate = [&](size_t i) {
    if (!first) {
      if (i > 0) {
        codegen_.builder_.CreateCall(codegen_.rt_.printChar, {codegen_.builder_.getInt32(' ')});
      }
      return;
    }
    llvm::BasicBlock *spaceBlock = codegen_.functionFrame().newBlock("print.space");
    llvm::BasicBlock *valueBlock = codegen_.functionFrame().newBlock("print.value");
    codegen_.builder_.CreateCondBr(codegen_.builder_.CreateLoad(codegen_.builder_.getInt1Ty(), first), valueBlock, spaceBlock);
    codegen_.functionFrame().switchTo(spaceBlock);
    codegen_.builder_.CreateCall(codegen_.rt_.printChar, {codegen_.builder_.getInt32(' ')});
    codegen_.builder_.CreateBr(valueBlock);
    codegen_.functionFrame().switchTo(valueBlock);
    codegen_.builder_.CreateStore(codegen_.builder_.getFalse(), first);
  };
  for (size_t i = 0; i < count; ++i) {
    const CodeGen::Operand &value = values[i];
    if (call->spreads(i) && value.type->isArray()) {
      // Each element, in a loop.
      const Type *element = value.type->inner();
      llvm::Value *length = codegen_.arrayLowerer().arrayLength(value.value);
      llvm::Value *indexSlot = codegen_.functionFrame().createStackSlot(codegen_.builder_.getInt64Ty(), "print.index");
      codegen_.builder_.CreateStore(codegen_.builder_.getInt64(0), indexSlot);
      llvm::BasicBlock *condBlock = codegen_.functionFrame().newBlock("print.cond");
      llvm::BasicBlock *bodyBlock = codegen_.functionFrame().newBlock("print.body");
      llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("print.end");
      codegen_.builder_.CreateBr(condBlock);
      codegen_.functionFrame().switchTo(condBlock);
      llvm::Value *index = codegen_.builder_.CreateLoad(codegen_.builder_.getInt64Ty(), indexSlot);
      codegen_.builder_.CreateCondBr(codegen_.builder_.CreateICmpULT(index, length), bodyBlock, endBlock);
      codegen_.functionFrame().switchTo(bodyBlock);
      separate(i);
      llvm::Value *at = codegen_.builder_.CreateLoad(codegen_.builder_.getInt64Ty(), indexSlot);
      llvm::Value *item = codegen_.builder_.CreateLoad(
          codegen_.moduleBuilder().lower(element), codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(value.value), element, at), "item");
      print(item, element);
      codegen_.builder_.CreateStore(codegen_.builder_.CreateAdd(at, codegen_.builder_.getInt64(1)), indexSlot);
      codegen_.builder_.CreateBr(condBlock);
      codegen_.functionFrame().switchTo(endBlock);
      codegen_.refCounts().dropOperand(value);
      continue;
    }
    separate(i);
    print(value.value, value.type);
    codegen_.refCounts().dropOperand(value);
  }
  codegen_.builder_.CreateCall(codegen_.rt_.printChar, {codegen_.builder_.getInt32('\n')});
  if (toStderr) {
    codegen_.builder_.CreateCall(codegen_.processLowerer().processFunction("tz_err_end", codegen_.builder_.getVoidTy(), {}));
  }
}

} // namespace tz
