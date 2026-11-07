#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

AssignLowerer::AssignLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *AssignLowerer::emitAssign(AssignExpr *assign, bool needResult) {
  this->lowering.orderAt(assign->loc());
  this->lowering.orderAt(assign->target->loc());
  if (assign->op && *assign->op == BinaryOp::Nullish) {
    return this->emitNullishAssign(assign, needResult);
  }
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  if (index && index->object->type->nonOptional()->isJson()) {
    return this->lowering.jsonLowerer().emitJsonAssign(assign, needResult);
  }
  if (member && member->object->type->nonOptional()->isJson()) {
    return this->lowering.jsonLowerer().emitJsonAssign(assign, needResult);
  }
  if (index && index->keyed()) {
    return this->lowering.objectLowerer().emitFieldAssign(assign, needResult);
  }
  if (index && index->object->type->isTypedArray()) {
    return this->lowering.bytesLowerer().emitBytesElementAssign(assign, needResult);
  }
  if (index) {
    return this->lowering.arrayLowerer().emitElementAssign(assign, needResult);
  }
  if (member) {
    return member->object->type->isObject() || member->object->type->isUnion()
               ? this->lowering.objectLowerer().emitFieldAssign(assign, needResult)
               : this->lowering.arrayLowerer().emitLengthAssign(assign, needResult);
  }

  const VarSymbol *var = cast<NameExpr>(assign->target)->var();
  llvm::Value *address = this->lowering.functionFrame().addressOf(var);
  const Type *type = assign->type;
  llvm::Value *value;
  if (!assign->op) {
    value = this->lowering.exprLowerer().emitExpr(assign->value);
  } else if (type->isString()) {
    Lowering::Operand old{this->lowering.builder_.CreateLoad(this->lowering.strTy_, address), false,
                          type};
    if (!AssignLowerer::cannotAssign(assign->value)) {
      this->lowering.refCounts().retainString(old.value);
      old.owned = true;
    }
    Lowering::Operand rhs = this->lowering.stringLowerer().emitConcatPart(assign->value, false);
    value = this->lowering.stringLowerer().concatOperands({old, rhs});
  } else if (type->isDecimal()) {
    llvm::Value *old = this->lowering.builder_.CreateLoad(this->lowering.decTy_, address);
    bool hold = !AssignLowerer::cannotAssign(assign->value);
    if (hold) {
      this->lowering.refCounts().retainValue(old, type);
    }
    value = this->lowering.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
    if (hold) {
      this->lowering.refCounts().releaseValue(old, type);
    }
  } else {
    llvm::Value *old =
        this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(type), address);
    llvm::Value *rhs = this->lowering.exprLowerer().emitExpr(assign->value);
    value = this->lowering.binaryLowerer().emitArithmetic(*assign->op, old, rhs, type,
                                                          assign->rangeProven);
  }
  this->lowering.storeLowerer().storeVar(var, address, value, true);
  if (!needResult) {
    return nullptr;
  }
  this->lowering.refCounts().retainValue(value, type);

  return value;
}

llvm::Value *AssignLowerer::emitNullishAssign(AssignExpr *assign, bool needResult) {
  const VarSymbol *var = cast<NameExpr>(assign->target)->var();
  const Type *varType = var->type;
  llvm::Value *address = this->lowering.functionFrame().addressOf(var);
  llvm::Value *old =
      this->lowering.builder_.CreateLoad(this->lowering.moduleBuilder().lower(varType), address);
  llvm::BasicBlock *keepBlock = this->lowering.functionFrame().newBlock("nullish.keep");
  llvm::BasicBlock *assignBlock = this->lowering.functionFrame().newBlock("nullish.assign");
  llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("nullish.end");
  llvm::Value *has = varType->nonOptional()->isJson()
                         ? this->lowering.jsonLowerer().jsonPresent(old)
                         : this->lowering.optionalLowerer().optionalHas(old, varType);
  this->lowering.builder_.CreateCondBr(has, keepBlock, assignBlock);

  const Type *resultType = assign->type;
  this->lowering.functionFrame().switchTo(keepBlock);
  llvm::Value *kept = nullptr;
  if (needResult) {
    this->lowering.refCounts().retainValue(old, varType);
    kept = resultType->isOptional() ? old
                                    : this->lowering.optionalLowerer().optionalValue(old, varType);
  }
  llvm::BasicBlock *keepEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(assignBlock);
  llvm::Value *value = this->lowering.exprLowerer().emitExpr(assign->value);
  const Type *valueType = assign->value->type;
  llvm::Value *stored = valueType->isOptional()
                            ? value
                            : this->lowering.optionalLowerer().makeOptional(
                                  value, this->lowering.builder_.getTrue(), varType);
  this->lowering.storeLowerer().storeVar(var, address, stored, true);
  llvm::Value *assigned = nullptr;
  if (needResult) {
    this->lowering.refCounts().retainValue(stored, varType);
    assigned = resultType->isOptional()
                   ? stored
                   : this->lowering.optionalLowerer().optionalValue(stored, varType);
  }
  llvm::BasicBlock *assignEnd = this->lowering.builder_.GetInsertBlock();
  this->lowering.builder_.CreateBr(endBlock);

  this->lowering.functionFrame().switchTo(endBlock);
  if (!needResult) {
    return nullptr;
  }
  llvm::PHINode *phi =
      this->lowering.builder_.CreatePHI(this->lowering.moduleBuilder().lower(resultType), 2);
  phi->addIncoming(kept, keepEnd);
  phi->addIncoming(assigned, assignEnd);

  return phi;
}

llvm::Value *AssignLowerer::emitUpdate(UpdateExpr *update) {
  this->lowering.orderAt(update->loc());
  this->lowering.orderAt(update->target->loc());
  if (auto *index = dyn_cast<IndexExpr>(update->target);
      index && index->object->type->isTypedArray()) {
    return this->lowering.bytesLowerer().emitBytesUpdate(update);
  }
  llvm::Value *address;
  Lowering::Operand holder{nullptr, false, nullptr};
  auto *keyed = dyn_cast<IndexExpr>(update->target);
  if (keyed && keyed->keyed()) {
    holder = this->lowering.refCounts().emitOperand(keyed->object,
                                                    AssignLowerer::cannotAssign(keyed->index));
    address = this->lowering.objectLowerer().keyedFieldAddress(
        holder.value, keyed->object->type, this->lowering.exprLowerer().emitExpr(keyed->index),
        keyed->index->type);
  } else if (auto *index = dyn_cast<IndexExpr>(update->target)) {
    holder = this->lowering.refCounts().emitOperand(index->object,
                                                    AssignLowerer::cannotAssign(index->index));
    llvm::Value *position = this->lowering.exprLowerer().emitExpr(index->index);
    address = this->lowering.arrayLowerer().elementAddress(
        this->lowering.arrayLowerer().arrayData(holder.value), update->type, position);
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    holder = this->lowering.refCounts().emitOperand(member->object, true);
    address = this->lowering.objectLayouts().fieldAddress(holder.value, member->object->type,
                                                          member->name);
  } else {
    address = this->lowering.functionFrame().addressOf(cast<NameExpr>(update->target)->var());
  }
  if (update->type->isDecimal()) {
    llvm::Value *result = this->lowering.decimalLowerer().emitDecimalUpdate(update, address);
    if (holder.value) {
      this->lowering.refCounts().dropOperand(holder);
    }
    return result;
  }
  llvm::Type *type = this->lowering.moduleBuilder().lower(update->type);
  llvm::Value *old = this->lowering.builder_.CreateLoad(type, address);
  llvm::Value *updated;
  if (update->type->isFloat()) {
    llvm::Value *one = llvm::ConstantFP::get(type, 1.0);
    updated = update->increment ? this->lowering.builder_.CreateFAdd(old, one)
                                : this->lowering.builder_.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(type, 1);
    updated = update->increment ? this->lowering.builder_.CreateAdd(old, one)
                                : this->lowering.builder_.CreateSub(old, one);
  }
  this->lowering.builder_.CreateStore(updated, address);
  if (holder.value) {
    this->lowering.refCounts().dropOperand(holder);
  }

  return update->prefix ? updated : old;
}

bool AssignLowerer::cannotAssign(const Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::RegexLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::Name:
  case Expr::Kind::Function:
    return true;
  case Expr::Kind::Template:
    return llvm::all_of(cast<TemplateExpr>(expr)->exprs, cannotAssign);
  case Expr::Kind::ArrayLit:
    return llvm::all_of(cast<ArrayLitExpr>(expr)->elements, [](const ArrayLitExpr::Element &e) {
      return AssignLowerer::cannotAssign(e.expr) && !(e.spread && e.expr->type->isIterator());
    });
  case Expr::Kind::ObjectLit:
    return llvm::all_of(cast<ObjectLitExpr>(expr)->props, [](const ObjectLitExpr::Property &p) {
      return AssignLowerer::cannotAssign(p.value);
    });
  case Expr::Kind::Member:
    return AssignLowerer::cannotAssign(cast<MemberExpr>(expr)->object);
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    return AssignLowerer::cannotAssign(index->object) && AssignLowerer::cannotAssign(index->index);
  }
  case Expr::Kind::Unary:
    return AssignLowerer::cannotAssign(cast<UnaryExpr>(expr)->operand);
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    return AssignLowerer::cannotAssign(bin->lhs) && AssignLowerer::cannotAssign(bin->rhs);
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    return AssignLowerer::cannotAssign(cond->cond) && AssignLowerer::cannotAssign(cond->thenExpr) &&
           AssignLowerer::cannotAssign(cond->elseExpr);
  }
  case Expr::Kind::Cast:
    return AssignLowerer::cannotAssign(cast<CastExpr>(expr)->operand);
  case Expr::Kind::Sequence:
    return llvm::all_of(cast<SequenceExpr>(expr)->exprs, cannotAssign);
  case Expr::Kind::OptionalChain:
    return AssignLowerer::cannotAssign(cast<OptionalChainExpr>(expr)->chain);
  case Expr::Kind::New:
    return !cast<NewExpr>(expr)->cls() && !cast<NewExpr>(expr)->reflect() &&
           llvm::all_of(cast<NewExpr>(expr)->args, cannotAssign);
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    if (call->builtin() == Builtin::None ||
        (call->builtin() >= Builtin::ArrForEach && call->builtin() <= Builtin::ArrSortWith) ||
        isResultCallback(call->builtin()) || call->builtin() == Builtin::PromiseNew ||
        call->builtin() == Builtin::IterNext || call->builtin() == Builtin::ArrayFrom ||
        isIteratorSink(call->builtin()) || call->builtin() == Builtin::StrReplaceRegexFn ||
        call->builtin() == Builtin::MapForEach || call->builtin() == Builtin::DictGroupBy ||
        call->builtin() == Builtin::DictNew ||
        (call->builtin() == Builtin::BytesFromArray && call->args[0]->type->isIterator())) {
      return false;
    }
    if (!llvm::all_of(call->args, cannotAssign)) {
      return false;
    }
    auto *member = dyn_cast<MemberExpr>(call->callee);
    return !member || call->builtin() == Builtin::ConsoleLog || isMathBuiltin(call->builtin()) ||
           AssignLowerer::cannotAssign(member->object);
  }
  case Expr::Kind::Assign:
  case Expr::Kind::Update:
  case Expr::Kind::Yield:
  case Expr::Kind::Await:
  case Expr::Kind::Class:
    return false;
  }

  return false;
}

}
