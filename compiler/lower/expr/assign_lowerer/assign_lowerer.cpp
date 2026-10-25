#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
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

AssignLowerer::AssignLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *AssignLowerer::emitAssign(AssignExpr *assign, bool needResult) {
  codegen_.orderAt(assign->loc());
  codegen_.orderAt(assign->target->loc());
  if (assign->op && *assign->op == BinaryOp::Nullish) {
    return emitNullishAssign(assign, needResult);
  }
  if (auto *index = dyn_cast<IndexExpr>(assign->target);
      index && index->object->type->nonOptional()->isJson()) {
    return codegen_.jsonLowerer().emitJsonAssign(assign, needResult);
  }
  if (auto *member = dyn_cast<MemberExpr>(assign->target);
      member && member->object->type->nonOptional()->isJson()) {
    return codegen_.jsonLowerer().emitJsonAssign(assign, needResult);
  }
  if (auto *index = dyn_cast<IndexExpr>(assign->target); index && index->keyed) {
    return codegen_.objectLowerer().emitFieldAssign(assign, needResult);
  }
  if (auto *index = dyn_cast<IndexExpr>(assign->target);
      index && index->object->type->isTypedArray()) {
    return codegen_.bytesLowerer().emitBytesElementAssign(assign, needResult);
  }
  if (isa<IndexExpr>(assign->target)) {
    return codegen_.arrayLowerer().emitElementAssign(assign, needResult);
  }
  if (auto *member = dyn_cast<MemberExpr>(assign->target)) {
    return member->object->type->isObject() || member->object->type->isUnion()
               ? codegen_.objectLowerer().emitFieldAssign(assign, needResult)
               : codegen_.arrayLowerer().emitLengthAssign(assign, needResult);
  }

  const VarSymbol *var = cast<NameExpr>(assign->target)->var;
  llvm::Value *address = codegen_.functionFrame().addressOf(var);
  const Type *type = assign->type;
  llvm::Value *value;
  if (!assign->op) {
    value = codegen_.exprLowerer().emitExpr(assign->value);
  } else if (type->isString()) {
    // `s += x`: the variable's own value is borrowed unless evaluating `x`
    // could reassign the variable.
    CodeGen::Operand old{codegen_.builder_.CreateLoad(codegen_.strTy_, address), false, type};
    if (!cannotAssign(assign->value)) {
      codegen_.refCounts().retainString(old.value);
      old.owned = true;
    }
    CodeGen::Operand rhs = codegen_.stringLowerer().emitConcatPart(assign->value, /*canBorrow=*/false);
    value = codegen_.stringLowerer().concatOperands({old, rhs});
  } else if (type->isDecimal()) {
    // As for strings, the old value is borrowed unless evaluating the right
    // side could reassign the variable.
    llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.decTy_, address);
    bool hold = !cannotAssign(assign->value);
    if (hold) {
      codegen_.refCounts().retainValue(old, type);
    }
    value = codegen_.decimalLowerer().emitDecimalCompound(*assign->op, old, assign->value);
    if (hold) {
      codegen_.refCounts().releaseValue(old, type);
    }
  } else {
    // As in JavaScript, the old value is read before the right side runs.
    llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), address);
    llvm::Value *rhs = codegen_.exprLowerer().emitExpr(assign->value);
    value = codegen_.binaryLowerer().emitArithmetic(*assign->op, old, rhs, type);
  }
  codegen_.storeLowerer().storeVar(var, address, value, /*replacing=*/true);
  if (!needResult) {
    return nullptr;
  }
  codegen_.refCounts().retainValue(value, type);

  return value;
}

// `x ??= v`
llvm::Value *AssignLowerer::emitNullishAssign(AssignExpr *assign, bool needResult) {
  const VarSymbol *var = cast<NameExpr>(assign->target)->var;
  const Type *varType = var->type;
  llvm::Value *address = codegen_.functionFrame().addressOf(var);
  llvm::Value *old = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(varType), address);
  llvm::BasicBlock *keepBlock = codegen_.functionFrame().newBlock("nullish.keep");
  llvm::BasicBlock *assignBlock = codegen_.functionFrame().newBlock("nullish.assign");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("nullish.end");
  // A Json that holds null is replaced too, as in JavaScript.
  llvm::Value *has =
      varType->nonOptional()->isJson() ? codegen_.jsonLowerer().jsonPresent(old) : codegen_.optionalLowerer().optionalHas(old, varType);
  codegen_.builder_.CreateCondBr(has, keepBlock, assignBlock);

  const Type *resultType = assign->type;
  codegen_.functionFrame().switchTo(keepBlock);
  llvm::Value *kept = nullptr;
  if (needResult) {
    codegen_.refCounts().retainValue(old, varType);
    kept = resultType->isOptional() ? old : codegen_.optionalLowerer().optionalValue(old, varType);
  }
  llvm::BasicBlock *keepEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(assignBlock);
  llvm::Value *value = codegen_.exprLowerer().emitExpr(assign->value);
  const Type *valueType = assign->value->type;
  llvm::Value *stored =
      valueType->isOptional() ? value : codegen_.optionalLowerer().makeOptional(value, codegen_.builder_.getTrue(), varType);
  codegen_.storeLowerer().storeVar(var, address, stored, /*replacing=*/true);
  llvm::Value *assigned = nullptr;
  if (needResult) {
    codegen_.refCounts().retainValue(stored, varType);
    assigned = resultType->isOptional() ? stored : codegen_.optionalLowerer().optionalValue(stored, varType);
  }
  llvm::BasicBlock *assignEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(endBlock);
  if (!needResult) {
    return nullptr;
  }
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(resultType), 2);
  phi->addIncoming(kept, keepEnd);
  phi->addIncoming(assigned, assignEnd);

  return phi;
}

llvm::Value *AssignLowerer::emitUpdate(UpdateExpr *update) {
  codegen_.orderAt(update->loc());
  codegen_.orderAt(update->target->loc());
  if (auto *index = dyn_cast<IndexExpr>(update->target);
      index && index->object->type->isTypedArray()) {
    return codegen_.bytesLowerer().emitBytesUpdate(update);
  }
  llvm::Value *address;
  CodeGen::Operand holder{nullptr, false, nullptr};
  if (auto *keyed = dyn_cast<IndexExpr>(update->target); keyed && keyed->keyed) {
    // `o[k]++` with a key of literals.
    holder = codegen_.refCounts().emitOperand(keyed->object, cannotAssign(keyed->index));
    address = codegen_.objectLowerer().keyedFieldAddress(holder.value, keyed->object->type, codegen_.exprLowerer().emitExpr(keyed->index),
                                keyed->index->type);
  } else if (auto *index = dyn_cast<IndexExpr>(update->target)) {
    // `a[i]++`: Sema proved the index in range.
    holder = codegen_.refCounts().emitOperand(index->object, cannotAssign(index->index));
    llvm::Value *position = codegen_.exprLowerer().emitExpr(index->index);
    address = codegen_.arrayLowerer().elementAddress(codegen_.arrayLowerer().arrayData(holder.value), update->type, position);
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    // `o.count++`: the object holds the number.
    holder = codegen_.refCounts().emitOperand(member->object, /*canBorrow=*/true);
    address = codegen_.objectLayouts().fieldAddress(holder.value, member->object->type, member->name);
  } else {
    address = codegen_.functionFrame().addressOf(cast<NameExpr>(update->target)->var);
  }
  if (update->type->isDecimal()) {
    llvm::Value *result = codegen_.decimalLowerer().emitDecimalUpdate(update, address);
    if (holder.value) {
      codegen_.refCounts().dropOperand(holder);
    }
    return result;
  }
  llvm::Type *type = codegen_.moduleBuilder().lower(update->type);
  llvm::Value *old = codegen_.builder_.CreateLoad(type, address);
  llvm::Value *updated;
  if (update->type->isFloat()) {
    llvm::Value *one = llvm::ConstantFP::get(type, 1.0);
    updated = update->increment ? codegen_.builder_.CreateFAdd(old, one) : codegen_.builder_.CreateFSub(old, one);
  } else {
    llvm::Value *one = llvm::ConstantInt::get(type, 1);
    updated = update->increment ? codegen_.builder_.CreateAdd(old, one) : codegen_.builder_.CreateSub(old, one);
  }
  codegen_.builder_.CreateStore(updated, address);
  if (holder.value) {
    codegen_.refCounts().dropOperand(holder);
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
  case Expr::Kind::Function: // creating a closure runs none of its code
    return true;
  case Expr::Kind::Template:
    return llvm::all_of(cast<TemplateExpr>(expr)->exprs, cannotAssign);
  case Expr::Kind::ArrayLit:
    // Spreading an iterator runs a generator or the callbacks of helpers.
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
  case Expr::Kind::New: // a constructor runs code
    return !cast<NewExpr>(expr)->cls && !cast<NewExpr>(expr)->reflect &&
           llvm::all_of(cast<NewExpr>(expr)->args, cannotAssign);
  case Expr::Kind::Call: {
    // A user function may assign to globals and fields, and so may the
    // callback of a built-in, and a generator that next() or a consumer of
    // an iterator runs; other built-ins only evaluate their operands (array
    // methods change arrays, not variables).
    auto *call = cast<CallExpr>(expr);
    if (call->builtin == Builtin::None ||
        (call->builtin >= Builtin::ArrForEach && call->builtin <= Builtin::ArrSortWith) ||
        isResultCallback(call->builtin) || call->builtin == Builtin::SafeTry ||
        call->builtin == Builtin::PromiseNew ||     // runs its executor
        call->builtin == Builtin::ResultAsyncMap || // asyncMap of a Result runs its function
        call->builtin == Builtin::ResultAsyncAndThen || call->builtin == Builtin::IterNext ||
        call->builtin == Builtin::ArrayFrom || isIteratorSink(call->builtin) ||
        call->builtin == Builtin::StrReplaceRegexFn || call->builtin == Builtin::MapForEach ||
        call->builtin == Builtin::DictGroupBy || call->builtin == Builtin::DictNew ||
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
  case Expr::Kind::Yield: // whoever iterates runs any code
  case Expr::Kind::Await: // so do the other tasks meanwhile
  case Expr::Kind::Class: // its static initializers run
    return false;
  }

  return false;
}

} // namespace tz
