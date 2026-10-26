#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
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

RefCounts::RefCounts(Lowering &lowering) : lowering_(lowering) {}

void RefCounts::retainValue(llvm::Value *value, const Type *type) {
  if (isa<llvm::Constant>(value)) {
    return;
  }
  switch (type->kind()) {
  case Type::Kind::String:
    lowering_.builder_.CreateCall(lowering_.retainFn_, {value});
    break;
  case Type::Kind::Array:
    lowering_.builder_.CreateCall(lowering_.memory().refMayBeMember(type) ? lowering_.refRetainFn_
                                                                          : lowering_.arrRetainFn_,
                                  {value});
    break;
  case Type::Kind::Function:
    lowering_.builder_.CreateCall(lowering_.memory().refMayBeMember(type)
                                      ? lowering_.refRetainFn_
                                      : lowering_.plainObjRetainFn_,
                                  {lowering_.builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Json:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Unknown:
    lowering_.builder_.CreateCall(lowering_.memory().refMayBeMember(type) ? lowering_.refRetainFn_
                                                                          : lowering_.arrRetainFn_,
                                  {value});
    break;
  case Type::Kind::Decimal:
    lowering_.builder_.CreateCall(lowering_.decimalLowerer().decimalRefCount(true), {value});
    break;
  case Type::Kind::Union:
    if (UnionLayout::isObjectUnion(type)) {
      lowering_.builder_.CreateCall(
          lowering_.memory().refMayBeMember(type) ? lowering_.refRetainFn_ : lowering_.arrRetainFn_,
          {value});
    } else if (type->isTagged()) {
      lowering_.unionLayout().countTagged(value, type, true);
    }
    break;
  case Type::Kind::Optional:
    if (type->inner()->isManaged()) {
      retainValue(lowering_.optionalLowerer().optionalValue(value, type), type->inner());
    }
    break;
  case Type::Kind::Result:
    if (!type->isManaged()) {
      break;
    }
    retainValue(lowering_.resultLowerer().resultHalf(value, type, false), type->okType());
    retainValue(lowering_.resultLowerer().resultHalf(value, type, true), type->errType());
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
    lowering_.builder_.CreateCall(lowering_.releaseFn_, {value});
    break;
  case Type::Kind::Array:
    lowering_.builder_.CreateCall(
        lowering_.memory().refMayBeMember(type)
            ? lowering_.groupLowerer().groupAwareRelease(lowering_.arrReleaseFn_)
            : lowering_.arrReleaseFn_,
        {value});
    break;
  case Type::Kind::Function:
    lowering_.builder_.CreateCall(
        lowering_.memory().refMayBeMember(type)
            ? lowering_.groupLowerer().groupAwareRelease(lowering_.plainObjReleaseFn_)
            : lowering_.plainObjReleaseFn_,
        {lowering_.builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object: {
    llvm::Function *release = type->isAnyObject() || lowering_.hasSubtypes(type) ||
                                      type->isTimer() || type->isRuntimeObject() ||
                                      type->isCrypto() ||
                                      (type->classDecl() && type->classDecl()->builtin)
                                  ? lowering_.plainInstanceReleaseFn_
                                  : lowering_.objectLayouts().objectLayout(type).release;
    lowering_.builder_.CreateCall(lowering_.memory().refMayBeMember(type)
                                      ? lowering_.groupLowerer().groupAwareRelease(release)
                                      : release,
                                  {value});
    break;
  }
  case Type::Kind::Decimal:
    lowering_.builder_.CreateCall(lowering_.decimalLowerer().decimalRefCount(false), {value});
    break;
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Unknown:
    lowering_.builder_.CreateCall(
        lowering_.memory().refMayBeMember(type)
            ? lowering_.groupLowerer().groupAwareRelease(lowering_.plainInstanceReleaseFn_)
            : lowering_.plainInstanceReleaseFn_,
        {value});
    break;
  case Type::Kind::Union:
    if (UnionLayout::isObjectUnion(type)) {
      lowering_.builder_.CreateCall(
          lowering_.memory().refMayBeMember(type)
              ? lowering_.groupLowerer().groupAwareRelease(lowering_.plainInstanceReleaseFn_)
              : lowering_.plainInstanceReleaseFn_,
          {value});
    } else if (type->isTagged()) {
      lowering_.unionLayout().countTagged(value, type, false);
    }
    break;
  case Type::Kind::Json:
    lowering_.builder_.CreateCall(lowering_.jsonLowerer().jsonFunction(
                                      "tz_json_release_value", lowering_.builder_.getVoidTy(),
                                      {lowering_.builder_.getPtrTy()}),
                                  {value});
    break;
  case Type::Kind::Optional:
    if (type->inner()->isManaged()) {
      releaseValue(lowering_.optionalLowerer().optionalValue(value, type), type->inner());
    }
    break;
  case Type::Kind::Result:
    if (!type->isManaged()) {
      break;
    }
    releaseValue(lowering_.resultLowerer().resultHalf(value, type, false), type->okType());
    releaseValue(lowering_.resultLowerer().resultHalf(value, type, true), type->errType());
    break;
  default:
    break;
  }
}

void RefCounts::retainString(llvm::Value *str) { retainValue(str, Type::string()); }

void RefCounts::releaseString(llvm::Value *str) { releaseValue(str, Type::string()); }

Lowering::Operand RefCounts::emitOperand(Expr *expr, bool canBorrow) {
  if (canBorrow) {
    if (llvm::Value *value = emitBorrowed(expr)) {
      return {value, false, expr->type};
    }
  }

  return {lowering_.exprLowerer().emitExpr(expr), true, expr->type};
}

llvm::Value *RefCounts::emitBorrowed(Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var) {
    return lowering_.envLowerer().emitVarValue(name->var, true);
  }
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->operand->type->isOptional() &&
                                             conv->type == conv->operand->type->inner()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    if (!value) {
      return nullptr;
    }
    if (conv->type->isString()) {
      return lowering_.builder_.CreateSelect(
          lowering_.optionalLowerer().optionalHas(value, conv->operand->type),
          lowering_.optionalLowerer().optionalValue(value, conv->operand->type),
          lowering_.stringLowerer().stringLiteral(u"undefined"));
    }
    return lowering_.optionalLowerer().optionalValue(value, conv->operand->type);
  }
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->type->isObject() &&
                                             conv->operand->type->nonOptional()->isObject()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    if (value && conv->operand->type->isOptional()) {
      value = lowering_.optionalLowerer().optionalValue(value, conv->operand->type);
    }
    return value;
  }
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && !conv->shortCircuit &&
      UnionLowerer::narrowsUnion(conv->operand->type, conv->type)) {
    llvm::Value *value = emitBorrowed(conv->operand);
    return value ? lowering_.exprLowerer().emitConversion(value, conv->operand->type, conv->type)
                 : nullptr;
  }
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && conv->type->isResult() && conv->operand->type->isResult()) {
    llvm::Value *value = emitBorrowed(conv->operand);
    return value ? lowering_.exprLowerer().emitConversion(value, conv->operand->type, conv->type)
                 : nullptr;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || !member->object->type->isObject() || member->object->type->isBytes() ||
      member->method) {
    return nullptr;
  }
  if (const Lowering::InterfaceMember *method = lowering_.inheritLowerer().dispatchedMember(
          member->object->type, member->name, member->signature);
      method && method->callable) {
    return nullptr;
  }
  llvm::Value *object = emitBorrowed(member->object);
  if (!object) {
    return nullptr;
  }

  return lowering_.builder_.CreateLoad(
      lowering_.moduleBuilder().lower(member->type),
      lowering_.objectLayouts().fieldAddress(object, member->object->type, member->name),
      member->name);
}

void RefCounts::dropOperand(const Lowering::Operand &operand) {
  if (operand.owned) {
    releaseValue(operand.value, operand.type);
  }
}

}
