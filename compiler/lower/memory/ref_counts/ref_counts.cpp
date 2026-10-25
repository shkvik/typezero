#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
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

RefCounts::RefCounts(CodeGen &codegen) : codegen_(codegen) {}

void RefCounts::retainValue(llvm::Value *value, const Type *type) {
  if (isa<llvm::Constant>(value)) { // literals, empty slots
    return;
  }
  switch (type->kind()) {
  case Type::Kind::String:
    codegen_.builder_.CreateCall(codegen_.retainFn_, {value});
    break;
  case Type::Kind::Array:
    codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.refRetainFn_ : codegen_.arrRetainFn_, {value});
    break;
  case Type::Kind::Function:
    codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.refRetainFn_ : codegen_.plainObjRetainFn_,
                        {codegen_.builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object: // the count is the first word, as for arrays
  case Type::Kind::Iterator:
  case Type::Kind::Json:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Unknown:
    codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.refRetainFn_ : codegen_.arrRetainFn_, {value});
    break;
  case Type::Kind::Decimal:
    codegen_.builder_.CreateCall(codegen_.decimalLowerer().decimalRefCount(/*retain=*/true), {value});
    break;
  case Type::Kind::Union:
    if (UnionLayout::isObjectUnion(type)) {
      codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.refRetainFn_ : codegen_.arrRetainFn_, {value});
    } else if (type->isTagged()) {
      codegen_.unionLayout().countTagged(value, type, /*retain=*/true);
    }
    break;
  case Type::Kind::Optional:
    // Undefined holds the zero value, which needs no counting.
    if (type->inner()->isManaged()) {
      retainValue(codegen_.optionalLowerer().optionalValue(value, type), type->inner());
    }
    break;
  case Type::Kind::Result:
    // So does the half not in use.
    if (!type->isManaged()) {
      break;
    }
    retainValue(codegen_.resultLowerer().resultHalf(value, type, /*error=*/false), type->okType());
    retainValue(codegen_.resultLowerer().resultHalf(value, type, /*error=*/true), type->errType());
    break;
  default:
    break;
  }
}

void RefCounts::releaseValue(llvm::Value *value, const Type *type) {
  if (isa<llvm::Constant>(value)) {
    return;
  }
  switch (type->kind()) {
  case Type::Kind::String:
    codegen_.builder_.CreateCall(codegen_.releaseFn_, {value});
    break;
  case Type::Kind::Array:
    codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.groupLowerer().groupAwareRelease(codegen_.arrReleaseFn_) : codegen_.arrReleaseFn_,
                        {value});
    break;
  case Type::Kind::Function:
    codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.groupLowerer().groupAwareRelease(codegen_.plainObjReleaseFn_)
                                             : codegen_.plainObjReleaseFn_,
                        {codegen_.builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object: {
    // An object of any type, in an instance of a generic function, one that
    // may be of a type down the declared chain, and an Error, which may be of
    // a class derived from it: through the drop function of its own layout.
    llvm::Function *release = type->isAnyObject() || codegen_.hasSubtypes(type) || type->isTimer() ||
                                      type->isRuntimeObject() || type->isCrypto() ||
                                      (type->classDecl() && type->classDecl()->builtin)
                                  ? codegen_.plainInstanceReleaseFn_
                                  : codegen_.objectLayouts().objectLayout(type).release;
    codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.groupLowerer().groupAwareRelease(release) : release, {value});
    break;
  }
  case Type::Kind::Decimal:
    codegen_.builder_.CreateCall(codegen_.decimalLowerer().decimalRefCount(/*retain=*/false), {value});
    break;
  case Type::Kind::Iterator: // through the drop function of its layout
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Unknown: // an object, or a box, which is one too
    codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.groupLowerer().groupAwareRelease(codegen_.plainInstanceReleaseFn_)
                                             : codegen_.plainInstanceReleaseFn_,
                        {value});
    break;
  case Type::Kind::Union:
    if (UnionLayout::isObjectUnion(type)) { // of any of the types: through its own layout
      codegen_.builder_.CreateCall(codegen_.memoryAnalysis().refMayBeMember(type) ? codegen_.groupLowerer().groupAwareRelease(codegen_.plainInstanceReleaseFn_)
                                               : codegen_.plainInstanceReleaseFn_,
                          {value});
    } else if (type->isTagged()) {
      codegen_.unionLayout().countTagged(value, type, /*retain=*/false);
    }
    break;
  case Type::Kind::Json:
    codegen_.builder_.CreateCall(
        codegen_.jsonLowerer().jsonFunction("tz_json_release_value", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}),
        {value});
    break;
  case Type::Kind::Optional:
    if (type->inner()->isManaged()) {
      releaseValue(codegen_.optionalLowerer().optionalValue(value, type), type->inner());
    }
    break;
  case Type::Kind::Result:
    if (!type->isManaged()) {
      break;
    }
    releaseValue(codegen_.resultLowerer().resultHalf(value, type, /*error=*/false), type->okType());
    releaseValue(codegen_.resultLowerer().resultHalf(value, type, /*error=*/true), type->errType());
    break;
  default:
    break;
  }
}

void RefCounts::retainString(llvm::Value *str) { retainValue(str, Type::string()); }

void RefCounts::releaseString(llvm::Value *str) { releaseValue(str, Type::string()); }

CodeGen::Operand RefCounts::emitOperand(Expr *expr, bool canBorrow) {
  if (canBorrow) {
    if (llvm::Value *value = emitBorrowed(expr)) {
      return {value, false, expr->type};
    }
  }

  return {codegen_.exprLowerer().emitExpr(expr), true, expr->type};
}

// The variable keeps the objects on the way alive, and the caller made sure
// that nothing assigns to them, or to the variable, while the value is used.
llvm::Value *RefCounts::emitBorrowed(Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var) {
    return codegen_.envLowerer().emitVarValue(name->var, /*borrow=*/true);
  }
  // A variable or a field narrowed to hold a value.
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->operand->type->isOptional() &&
                                             conv->type == conv->operand->type->inner()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    if (!value) {
      return nullptr;
    }
    // A `string | undefined` made a string (a part of a template or of `+`)
    // looks the same, and may not hold one: then it is "undefined", as in
    // JavaScript (see emitConversion).
    if (conv->type->isString()) {
      return codegen_.builder_.CreateSelect(codegen_.optionalLowerer().optionalHas(value, conv->operand->type),
                                   codegen_.optionalLowerer().optionalValue(value, conv->operand->type),
                                   codegen_.stringLowerer().stringLiteral(u"undefined"));
    }
    return codegen_.optionalLowerer().optionalValue(value, conv->operand->type);
  }
  // An object seen as a type up or down its declared chain (narrowed by
  // instanceof): the same reference.
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->type->isObject() &&
                                             conv->operand->type->nonOptional()->isObject()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    if (value && conv->operand->type->isOptional()) {
      value = codegen_.optionalLowerer().optionalValue(value, conv->operand->type);
    }
    return value;
  }
  // A variable or a field of a union narrowed to some of its members: the
  // same value, or the part of it that holds them, without counting.
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             UnionLowerer::narrowsUnion(conv->operand->type, conv->type)) {
    llvm::Value *value = emitBorrowed(conv->operand);
    return value ? codegen_.exprLowerer().emitConversion(value, conv->operand->type, conv->type) : nullptr;
  }
  // A Result variable narrowed to an Ok or an Err: the same halves, one of
  // them known to be empty.
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && conv->type->isResult() && conv->operand->type->isResult()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    return value ? codegen_.exprLowerer().emitConversion(value, conv->operand->type, conv->type) : nullptr;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || !member->object->type->isObject() || member->object->type->isBytes() ||
      member->method) {
    return nullptr;
  }
  if (const CodeGen::InterfaceMember *method =
          codegen_.inheritLowerer().dispatchedMember(member->object->type, member->name, member->signature);
      method && method->callable) {
    return nullptr; // a method of an interface, bound to the object when read
  }
  llvm::Value *object = emitBorrowed(member->object);
  if (!object) {
    return nullptr;
  }

  return codegen_.builder_.CreateLoad(
      codegen_.moduleBuilder().lower(member->type), codegen_.objectLayouts().fieldAddress(object, member->object->type, member->name), member->name);
}

void RefCounts::dropOperand(const CodeGen::Operand &operand) {
  if (operand.owned) {
    releaseValue(operand.value, operand.type);
  }
}

} // namespace tz
