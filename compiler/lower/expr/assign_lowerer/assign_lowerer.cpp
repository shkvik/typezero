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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

AssignLowerer::AssignLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *AssignLowerer::emitAssign(AssignExpr *assign, bool needResult) {
  lowering_.orderAt(assign->loc());
  lowering_.orderAt(assign->target->loc());
  if (assign->op && *assign->op == BinaryOp::Nullish) {
    return emitNullishAssign(assign, needResult);
  }
  if (auto *index = dyn_cast<IndexExpr>(assign->target);
      index && index->object->type->nonOptional()->isJson()) {
    return lowering_.jsonLowerer().emitJsonAssign(assign, needResult);
  }
  if (auto *member = dyn_cast<MemberExpr>(assign->target);
      member && member->object->type->nonOptional()->isJson()) {
    return lowering_.jsonLowerer().emitJsonAssign(assign, needResult);
  }
  if (auto *index = dyn_cast<IndexExpr>(assign->target); index && index->keyed) {
    return lowering_.objectLowerer().emitFieldAssign(assign, needResult);
  }
  if (auto *index = dyn_cast<IndexExpr>(assign->target);
      index && index->object->type->isTypedArray()) {
    return lowering_.bytesLowerer().emitBytesElementAssign(assign, needResult);
  }
  if (isa<IndexExpr>(assign->target)) {
    return lowering_.arrayLowerer().emitElementAssign(assign, needResult);
  }
  if (auto *member = dyn_cast<MemberExpr>(assign->target)) {
    return member->object->type->isObject() || member->object->type->isUnion()
               ? lowering_.objectLowerer().emitFieldAssign(assign, needResult)
               : lowering_.arrayLowerer().emitLengthAssign(assign, needResult);
  }

  const VarSymbol *var = cast<NameExpr>(assign->target)->var;
  llvm::Value *address = lowering_.functionFrame().addressOf(var);
  const Type *type = assign->type;
  llvm::Value *value;
  if (!assign->op) {
    value = lowering_.exprLowerer().emitExpr(assign->value);
  } else if (type->isString()) {
    Lowering::Operand old{lowering_.builder_.CreateLoad(lowering_.strTy_, address), false, type};
    if (!cannotAssign(assign->value)) {
      lowering_.refCounts().retainString(old.value);
      old.owned = true;
    }
    Lowering::Operand rhs = lowering_.stringLowerer().emitConcatPart(assign->value, false);
    value = lowering_.stringLowerer().concatOperands({old, rhs});
  } else if (type->isDecimal()) {
    llvm::Value *old = lowering_.builder_.CreateLoad(lowering_.decTy_, address);
    bool hold = !cannotAssign(assign->value);
    if (hold) {
      lowering_.refCounts().retainValue(old, type);
    }
    value = lowering_.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
    if (hold) {
      lowering_.refCounts().releaseValue(old, type);
    }
  } else {
    llvm::Value *old =
        lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type), address);
    llvm::Value *rhs = lowering_.exprLowerer().emitExpr(assign->value);
    value =
        lowering_.binaryLowerer().emitArithmetic(*assign->op, old, rhs, type, assign->rangeProven);
  }
  lowering_.storeLowerer().storeVar(var, address, value, true);
  if (!needResult) {
    return nullptr;
  }
  lowering_.refCounts().retainValue(value, type);

  return value;
}

llvm::Value *AssignLowerer::emitNullishAssign(AssignExpr *assign, bool needResult) {
  const VarSymbol *var = cast<NameExpr>(assign->target)->var;
  const Type *varType = var->type;
  llvm::Value *address = lowering_.functionFrame().addressOf(var);
  llvm::Value *old =
      lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(varType), address);
  llvm::BasicBlock *keepBlock = lowering_.functionFrame().newBlock("nullish.keep");
  llvm::BasicBlock *assignBlock = lowering_.functionFrame().newBlock("nullish.assign");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("nullish.end");
  llvm::Value *has = varType->nonOptional()->isJson()
                         ? lowering_.jsonLowerer().jsonPresent(old)
                         : lowering_.optionalLowerer().optionalHas(old, varType);
  lowering_.builder_.CreateCondBr(has, keepBlock, assignBlock);

  const Type *resultType = assign->type;
  lowering_.functionFrame().switchTo(keepBlock);
  llvm::Value *kept = nullptr;
  if (needResult) {
    lowering_.refCounts().retainValue(old, varType);
    kept = resultType->isOptional() ? old : lowering_.optionalLowerer().optionalValue(old, varType);
  }
  llvm::BasicBlock *keepEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(assignBlock);
  llvm::Value *value = lowering_.exprLowerer().emitExpr(assign->value);
  const Type *valueType = assign->value->type;
  llvm::Value *stored =
      valueType->isOptional()
          ? value
          : lowering_.optionalLowerer().makeOptional(value, lowering_.builder_.getTrue(), varType);
  lowering_.storeLowerer().storeVar(var, address, stored, true);
  llvm::Value *assigned = nullptr;
  if (needResult) {
    lowering_.refCounts().retainValue(stored, varType);
    assigned = resultType->isOptional()
                   ? stored
                   : lowering_.optionalLowerer().optionalValue(stored, varType);
  }
  llvm::BasicBlock *assignEnd = lowering_.builder_.GetInsertBlock();
  lowering_.builder_.CreateBr(endBlock);

  lowering_.functionFrame().switchTo(endBlock);
  if (!needResult) {
    return nullptr;
  }
  llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.moduleBuilder().lower(resultType), 2);
  phi->addIncoming(kept, keepEnd);
  phi->addIncoming(assigned, assignEnd);

  return phi;
}

llvm::Value *AssignLowerer::emitUpdate(UpdateExpr *update) {
  lowering_.orderAt(update->loc());
  lowering_.orderAt(update->target->loc());
  if (auto *index = dyn_cast<IndexExpr>(update->target);
      index && index->object->type->isTypedArray()) {
    return lowering_.bytesLowerer().emitBytesUpdate(update);
  }
  llvm::Value *address;
  Lowering::Operand holder{nullptr, false, nullptr};
  if (auto *keyed = dyn_cast<IndexExpr>(update->target); keyed && keyed->keyed) {
    holder = lowering_.refCounts().emitOperand(keyed->object, cannotAssign(keyed->index));
    address = lowering_.objectLowerer().keyedFieldAddress(
        holder.value, keyed->object->type, lowering_.exprLowerer().emitExpr(keyed->index),
        keyed->index->type);
  } else if (auto *index = dyn_cast<IndexExpr>(update->target)) {
    holder = lowering_.refCounts().emitOperand(index->object, cannotAssign(index->index));
    llvm::Value *position = lowering_.exprLowerer().emitExpr(index->index);
    address = lowering_.arrayLowerer().elementAddress(
        lowering_.arrayLowerer().arrayData(holder.value), update->type, position);
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    holder = lowering_.refCounts().emitOperand(member->object, true);
    address =
        lowering_.objectLayouts().fieldAddress(holder.value, member->object->type, member->name);
  } else {
    address = lowering_.functionFrame().addressOf(cast<NameExpr>(update->target)->var);
  }
  if (update->type->isDecimal()) {
    llvm::Value *result = lowering_.decimalLowerer().emitDecimalUpdate(update, address);
    if (holder.value) {
      lowering_.refCounts().dropOperand(holder);
    }
    return result;
  }
  llvm::Type *type = lowering_.moduleBuilder().lower(update->type);
  llvm::Value *old = lowering_.builder_.CreateLoad(type, address);
  llvm::Value *updated;
  if (update->type->isFloat()) {
    llvm::Value *one = llvm::ConstantFP::get(type, 1.0);
    updated = update->increment ? lowering_.builder_.CreateFAdd(old, one)
                                : lowering_.builder_.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(type, 1);
    updated = update->increment ? lowering_.builder_.CreateAdd(old, one)
                                : lowering_.builder_.CreateSub(old, one);
  }
  lowering_.builder_.CreateStore(updated, address);
  if (holder.value) {
    lowering_.refCounts().dropOperand(holder);
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
      return cannotAssign(e.expr) && !(e.spread && e.expr->type->isIterator());
    });
  case Expr::Kind::ObjectLit:
    return llvm::all_of(cast<ObjectLitExpr>(expr)->props,
                        [](const ObjectLitExpr::Property &p) { return cannotAssign(p.value); });
  case Expr::Kind::Member:
    return cannotAssign(cast<MemberExpr>(expr)->object);
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    return cannotAssign(index->object) && cannotAssign(index->index);
  }
  case Expr::Kind::Unary:
    return cannotAssign(cast<UnaryExpr>(expr)->operand);
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    return cannotAssign(bin->lhs) && cannotAssign(bin->rhs);
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    return cannotAssign(cond->cond) && cannotAssign(cond->thenExpr) && cannotAssign(cond->elseExpr);
  }
  case Expr::Kind::Cast:
    return cannotAssign(cast<CastExpr>(expr)->operand);
  case Expr::Kind::Sequence:
    return llvm::all_of(cast<SequenceExpr>(expr)->exprs, cannotAssign);
  case Expr::Kind::OptionalChain:
    return cannotAssign(cast<OptionalChainExpr>(expr)->chain);
  case Expr::Kind::New:
    return !cast<NewExpr>(expr)->cls && !cast<NewExpr>(expr)->reflect &&
           llvm::all_of(cast<NewExpr>(expr)->args, cannotAssign);
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    if (call->builtin == Builtin::None ||
        (call->builtin >= Builtin::ArrForEach && call->builtin <= Builtin::ArrSortWith) ||
        isResultCallback(call->builtin) || call->builtin == Builtin::PromiseNew ||
        call->builtin == Builtin::IterNext || call->builtin == Builtin::ArrayFrom ||
        isIteratorSink(call->builtin) || call->builtin == Builtin::StrReplaceRegexFn ||
        call->builtin == Builtin::MapForEach || call->builtin == Builtin::DictGroupBy ||
        call->builtin == Builtin::DictNew ||
        (call->builtin == Builtin::BytesFromArray && call->args[0]->type->isIterator())) {
      return false;
    }
    if (!llvm::all_of(call->args, cannotAssign)) {
      return false;
    }
    auto *member = dyn_cast<MemberExpr>(call->callee);
    return !member || call->builtin == Builtin::ConsoleLog || isMathBuiltin(call->builtin) ||
           cannotAssign(member->object);
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
