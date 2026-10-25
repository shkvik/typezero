#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
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

// A value held as a pointer, null for undefined in `T | undefined`: an
// object, an iterator, a Json, a dictionary or a Promise.
bool isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

} // namespace

TruthLowerer::TruthLowerer(CodeGen &codegen) : codegen_(codegen) {}

// `&&` and `||` evaluate their right side only when needed. When the left
// side decides, it is the result (Sema converted the right side to the type
// of the whole).
llvm::Value *TruthLowerer::emitLogical(BinaryExpr *bin) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *lhsType = bin->lhs->type;
  llvm::Value *lhs = codegen_.exprLowerer().emitExpr(bin->lhs);
  llvm::Value *truth = emitTruth(lhs, lhsType);
  llvm::Value *decided = decidedValue(bin, lhs); // used only when it decides
  llvm::BasicBlock *lhsEnd = codegen_.builder_.GetInsertBlock();
  llvm::BasicBlock *rhsBlock = codegen_.functionFrame().newBlock(isAnd ? "and.rhs" : "or.rhs");
  llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock(isAnd ? "and.end" : "or.end");
  if (isAnd) {
    codegen_.builder_.CreateCondBr(truth, rhsBlock, endBlock);
  } else {
    codegen_.builder_.CreateCondBr(truth, endBlock, rhsBlock);
  }

  codegen_.functionFrame().switchTo(rhsBlock);
  codegen_.refCounts().releaseValue(lhs, lhsType); // the right side gives the result
  llvm::Value *rhs = codegen_.exprLowerer().emitExpr(bin->rhs);
  llvm::BasicBlock *rhsEnd = codegen_.builder_.GetInsertBlock();
  codegen_.builder_.CreateBr(endBlock);

  codegen_.functionFrame().switchTo(endBlock);
  llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.moduleBuilder().lower(bin->type), 2);
  phi->addIncoming(decided, lhsEnd);
  phi->addIncoming(rhs, rhsEnd);

  return phi;
}

llvm::Value *TruthLowerer::decidedValue(BinaryExpr *bin, llvm::Value *lhs) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  const Type *from = bin->lhs->type, *to = bin->type;
  if (from->isBool() && to->isBool()) {
    return codegen_.builder_.getInt1(!isAnd);
  }
  const Type *base = from->nonOptional();
  // A falsy array, function or object is undefined, which holds nothing to
  // release.
  if (isAnd && (base->isArray() || base->isFunction() || isReference(base))) {
    return codegen_.optionalLowerer().undefinedValue(to);
  }
  if (from == to) {
    return lhs;
  }
  // A truthy `T | undefined` holds a value; the other side may be undefined.
  if (from->isOptional()) {
    return codegen_.optionalLowerer().optionalValue(lhs, from);
  }

  return codegen_.optionalLowerer().makeOptional(lhs, codegen_.builder_.getTrue(), to);
}

llvm::Value *TruthLowerer::emitTruth(llvm::Value *value, const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Bool:
    return value;
  case Type::Kind::Literal:
  case Type::Kind::Union:
    return codegen_.unionLowerer().emitUnionTruth(value, type);
  case Type::Kind::String:
    return codegen_.builder_.CreateICmpNE(codegen_.stringLowerer().stringLength(value), codegen_.builder_.getInt64(0));
  case Type::Kind::Optional: {
    llvm::Value *has = codegen_.optionalLowerer().optionalHas(value, type);
    const Type *inner = type->inner();
    if (!inner->isBool() && !inner->isString() && !inner->isLiteral() &&
        !(inner->isUnion() && !UnionLayout::isObjectUnion(inner))) {
      return has; // an array, a function or an object is always truthy
    }
    return codegen_.builder_.CreateAnd(has, emitTruth(codegen_.optionalLowerer().optionalValue(value, type), inner));
  }
  default:
    llvm_unreachable("Sema rejects testing other types");
  }
}

llvm::Value *TruthLowerer::emitTruthOf(Expr *expr) {
  // A narrowed variable holds a value: its own truth is the same, and it can
  // be borrowed.
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && !conv->shortCircuit && isa<NameExpr>(conv->operand) &&
      conv->operand->type->isOptional() && conv->type == conv->operand->type->inner()) {
    expr = conv->operand;
  }
  CodeGen::Operand value = codegen_.refCounts().emitOperand(expr, /*canBorrow=*/true);
  llvm::Value *truth = emitTruth(value.value, value.type);
  codegen_.refCounts().dropOperand(value);

  return truth;
}

} // namespace tz
