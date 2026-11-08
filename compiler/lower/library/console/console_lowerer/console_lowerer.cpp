#include "compiler/lower/library/console/console_lowerer/console_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

ConsoleLowerer::ConsoleLowerer(Lowering &owner)
    : lowering(owner) {}

void ConsoleLowerer::emitConsoleLog(CallExpr *call) {
  size_t count = call->args.size();
  std::vector<bool> laterCannotAssign(count, true);
  for (size_t i = count; i > 1; --i) {
    laterCannotAssign[i - 2] =
        laterCannotAssign[i - 1] && AssignLowerer::cannotAssign(call->args[i - 1]);
  }
  std::vector<Lowering::Operand> values;
  for (size_t i = 0; i < count; ++i) {
    if (call->args[i]->type->isUndefined()) {
      this->lowering.stmtLowerer().emitDiscarded(call->args[i]);
      values.push_back({nullptr, false, call->args[i]->type});
    } else {
      values.push_back(this->lowering.refCounts().emitOperand(call->args[i], laterCannotAssign[i]));
    }
  }

  bool toStderr = call->builtin() == Builtin::ConsoleError;
  if (toStderr) {
    this->lowering.builder.CreateCall(this->lowering.processLowerer().processFunction(
        "tz_err_begin", this->lowering.builder.getVoidTy(), {}));
  }
  auto print = [&](llvm::Value *value, const Type *type) {
    if (type->isUndefined() || type->isNever()) {
      llvm::Value *text = this->lowering.stringLowerer().stringLiteral(u"undefined");
      this->lowering.builder.CreateCall(this->lowering.rt_.printString,
                                        {this->lowering.stringLowerer().stringObj(text),
                                         this->lowering.stringLowerer().stringMeta(text)});
      return;
    }
    switch (type->kind()) {
    case Type::Kind::String:
      this->lowering.builder.CreateCall(this->lowering.rt_.printString,
                                        {this->lowering.stringLowerer().stringObj(value),
                                         this->lowering.stringLowerer().stringMeta(value)});
      break;
    case Type::Kind::Int:
      this->lowering.builder.CreateCall(type->isUnsigned() && type->bitWidth() == 64
                                            ? this->lowering.rt_.printU64
                                            : this->lowering.rt_.printI64,
                                        {this->lowering.numberLowerer().toI64(value, type)});
      break;
    case Type::Kind::Double:
      this->lowering.builder.CreateCall(this->lowering.rt_.printF64, {value});
      break;
    case Type::Kind::Bool:
      this->lowering.builder.CreateCall(
          this->lowering.rt_.printBool,
          {this->lowering.builder.CreateZExt(value, this->lowering.builder.getInt32Ty())});
      break;
    case Type::Kind::Decimal: {
      llvm::Type *i64 = this->lowering.builder.getInt64Ty();
      this->lowering.builder.CreateCall(
          this->lowering.decimalLowerer().decimalFunction(
              "tz_print_decimal", this->lowering.builder.getVoidTy(), {i64, i64}),
          {this->lowering.decimalLowerer().decimalCoef(value),
           this->lowering.decimalLowerer().decimalMeta(value)});
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
      this->lowering.builder.CreateCall(
          this->lowering.rt_.printValue,
          {this->lowering.typeDescriptors().typeDescriptor(type),
           this->lowering.functionFrame().spill(value, "print.value")});
      break;
    default:
      llvm_unreachable("Checker rejects other argument types");
    }
  };
  llvm::Value *first = nullptr;
  if (call->hasSpread()) {
    first = this->lowering.functionFrame().createStackSlot(this->lowering.builder.getInt1Ty(),
                                                           "print.first");
    this->lowering.builder.CreateStore(this->lowering.builder.getTrue(), first);
  }
  auto separate = [&](size_t i) {
    if (!first) {
      if (i > 0) {
        this->lowering.builder.CreateCall(this->lowering.rt_.printChar,
                                          {this->lowering.builder.getInt32(' ')});
      }
      return;
    }
    llvm::BasicBlock *spaceBlock = this->lowering.functionFrame().newBlock("print.space");
    llvm::BasicBlock *valueBlock = this->lowering.functionFrame().newBlock("print.value");
    this->lowering.builder.CreateCondBr(
        this->lowering.builder.CreateLoad(this->lowering.builder.getInt1Ty(), first), valueBlock,
        spaceBlock);
    this->lowering.functionFrame().switchTo(spaceBlock);
    this->lowering.builder.CreateCall(this->lowering.rt_.printChar,
                                      {this->lowering.builder.getInt32(' ')});
    this->lowering.builder.CreateBr(valueBlock);
    this->lowering.functionFrame().switchTo(valueBlock);
    this->lowering.builder.CreateStore(this->lowering.builder.getFalse(), first);
  };
  for (size_t i = 0; i < count; ++i) {
    const Lowering::Operand &value = values[i];
    if (call->spreads(i) && value.type->isArray()) {
      const Type *element = value.type->inner();
      llvm::Value *length = this->lowering.arrayLowerer().arrayLength(value.value);
      llvm::Value *indexSlot = this->lowering.functionFrame().createStackSlot(
          this->lowering.builder.getInt64Ty(), "print.index");
      this->lowering.builder.CreateStore(this->lowering.builder.getInt64(0), indexSlot);
      llvm::BasicBlock *condBlock = this->lowering.functionFrame().newBlock("print.cond");
      llvm::BasicBlock *bodyBlock = this->lowering.functionFrame().newBlock("print.body");
      llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("print.end");
      this->lowering.builder.CreateBr(condBlock);
      this->lowering.functionFrame().switchTo(condBlock);
      llvm::Value *index =
          this->lowering.builder.CreateLoad(this->lowering.builder.getInt64Ty(), indexSlot);
      this->lowering.builder.CreateCondBr(this->lowering.builder.CreateICmpULT(index, length),
                                          bodyBlock, endBlock);
      this->lowering.functionFrame().switchTo(bodyBlock);
      separate(i);
      llvm::Value *at =
          this->lowering.builder.CreateLoad(this->lowering.builder.getInt64Ty(), indexSlot);
      llvm::Value *item = this->lowering.builder.CreateLoad(
          this->lowering.moduleBuilder().lower(element),
          this->lowering.arrayLowerer().elementAddress(
              this->lowering.arrayLowerer().arrayData(value.value), element, at),
          "item");
      print(item, element);
      this->lowering.builder.CreateStore(
          this->lowering.builder.CreateAdd(at, this->lowering.builder.getInt64(1)), indexSlot);
      this->lowering.builder.CreateBr(condBlock);
      this->lowering.functionFrame().switchTo(endBlock);
      this->lowering.refCounts().dropOperand(value);
      continue;
    }
    separate(i);
    print(value.value, value.type);
    this->lowering.refCounts().dropOperand(value);
  }
  this->lowering.builder.CreateCall(this->lowering.rt_.printChar,
                                    {this->lowering.builder.getInt32('\n')});
  if (toStderr) {
    this->lowering.builder.CreateCall(this->lowering.processLowerer().processFunction(
        "tz_err_end", this->lowering.builder.getVoidTy(), {}));
  }
}

}
