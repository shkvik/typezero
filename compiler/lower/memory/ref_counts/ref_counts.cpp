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

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

RefCounts::RefCounts(Lowering &owner)
    : lowering(owner) {}

void RefCounts::retainValue(llvm::Value *value, const Type *type) {
  if (isa<llvm::Constant>(value)) {
    return;
  }
  switch (type->kind()) {
  case Type::Kind::String:
    this->lowering.builder_.CreateCall(this->lowering.retainFn_, {value});
    break;
  case Type::Kind::Array:
    this->lowering.builder_.CreateCall(this->lowering.memory().refMayCarryMember(type)
                                           ? this->lowering.refRetainFn_
                                           : this->lowering.arrRetainFn_,
                                       {value});
    break;
  case Type::Kind::Function:
    this->lowering.builder_.CreateCall(this->lowering.memory().refMayCarryMember(type)
                                           ? this->lowering.refRetainFn_
                                           : this->lowering.plainObjRetainFn_,
                                       {this->lowering.builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Json:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Unknown:
    this->lowering.builder_.CreateCall(this->lowering.memory().refMayCarryMember(type)
                                           ? this->lowering.refRetainFn_
                                           : this->lowering.arrRetainFn_,
                                       {value});
    break;
  case Type::Kind::Decimal:
    this->lowering.builder_.CreateCall(this->lowering.decimalLowerer().decimalRefCount(true),
                                       {value});
    break;
  case Type::Kind::Union:
    if (UnionLayout::isObjectUnion(type)) {
      this->lowering.builder_.CreateCall(this->lowering.memory().refMayCarryMember(type)
                                             ? this->lowering.refRetainFn_
                                             : this->lowering.arrRetainFn_,
                                         {value});
    } else if (type->isTagged()) {
      this->lowering.unionLayout().countTagged(value, type, true);
    }
    break;
  case Type::Kind::Optional:
    if (type->inner()->isManaged()) {
      this->retainValue(this->lowering.optionalLowerer().optionalValue(value, type), type->inner());
    }
    break;
  case Type::Kind::Result:
    if (!type->isManaged()) {
      break;
    }
    this->retainValue(this->lowering.resultLowerer().resultHalf(value, type, false),
                      type->okType());
    this->retainValue(this->lowering.resultLowerer().resultHalf(value, type, true),
                      type->errType());
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
    this->lowering.builder_.CreateCall(this->lowering.releaseFn_, {value});
    break;
  case Type::Kind::Array:
    this->lowering.builder_.CreateCall(
        this->lowering.memory().refMayCarryMember(type)
            ? this->lowering.groupLowerer().groupAwareRelease(this->lowering.arrReleaseFn_)
            : this->lowering.arrReleaseFn_,
        {value});
    break;
  case Type::Kind::Function:
    this->lowering.builder_.CreateCall(
        this->lowering.memory().refMayCarryMember(type)
            ? this->lowering.groupLowerer().groupAwareRelease(this->lowering.plainObjReleaseFn_)
            : this->lowering.plainObjReleaseFn_,
        {this->lowering.builder_.CreateExtractValue(value, 1)});
    break;
  case Type::Kind::Object: {
    llvm::Function *release = type->isAnyObject() || this->lowering.hasSubtypes(type) ||
                                      type->isTimer() || type->isRuntimeObject() ||
                                      type->isCrypto() ||
                                      (type->classDecl() && type->classDecl()->builtin())
                                  ? this->lowering.plainInstanceReleaseFn_
                                  : this->lowering.objectLayouts().objectLayout(type).release;
    this->lowering.builder_.CreateCall(
        this->lowering.memory().refMayCarryMember(type)
            ? this->lowering.groupLowerer().groupAwareRelease(release)
            : release,
        {value});
    break;
  }
  case Type::Kind::Decimal:
    this->lowering.builder_.CreateCall(this->lowering.decimalLowerer().decimalRefCount(false),
                                       {value});
    break;
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Unknown:
    this->lowering.builder_.CreateCall(this->lowering.memory().refMayCarryMember(type)
                                           ? this->lowering.groupLowerer().groupAwareRelease(
                                                 this->lowering.plainInstanceReleaseFn_)
                                           : this->lowering.plainInstanceReleaseFn_,
                                       {value});
    break;
  case Type::Kind::Union:
    if (UnionLayout::isObjectUnion(type)) {
      this->lowering.builder_.CreateCall(this->lowering.memory().refMayCarryMember(type)
                                             ? this->lowering.groupLowerer().groupAwareRelease(
                                                   this->lowering.plainInstanceReleaseFn_)
                                             : this->lowering.plainInstanceReleaseFn_,
                                         {value});
    } else if (type->isTagged()) {
      this->lowering.unionLayout().countTagged(value, type, false);
    }
    break;
  case Type::Kind::Json:
    this->lowering.builder_.CreateCall(
        this->lowering.jsonLowerer().jsonFunction("tz_json_release_value",
                                                  this->lowering.builder_.getVoidTy(),
                                                  {this->lowering.builder_.getPtrTy()}),
        {value});
    break;
  case Type::Kind::Optional:
    if (type->inner()->isManaged()) {
      this->releaseValue(this->lowering.optionalLowerer().optionalValue(value, type),
                         type->inner());
    }
    break;
  case Type::Kind::Result:
    if (!type->isManaged()) {
      break;
    }
    this->releaseValue(this->lowering.resultLowerer().resultHalf(value, type, false),
                       type->okType());
    this->releaseValue(this->lowering.resultLowerer().resultHalf(value, type, true),
                       type->errType());
    break;
  default:
    break;
  }
}

void RefCounts::retainString(llvm::Value *str) {
  this->retainValue(str, Type::string());
}

void RefCounts::releaseString(llvm::Value *str) {
  this->releaseValue(str, Type::string());
}

Lowering::Operand RefCounts::emitOperand(Expr *expr, bool canBorrow) {
  if (canBorrow) {
    llvm::Value *value = this->emitBorrowed(expr);
    if (value) {
      return {value, false, expr->type};
    }
  }

  return {this->lowering.exprLowerer().emitExpr(expr), true, expr->type};
}

llvm::Value *RefCounts::emitBorrowed(Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name && name->var()) {
    return this->lowering.envLowerer().emitVarValue(name->var(), true);
  }
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->operand->type->isOptional() &&
                                             conv->type == conv->operand->type->inner()) {
    llvm::Value *value = this->emitBorrowed(conv->operand);
    if (!value) {
      return nullptr;
    }
    if (conv->type->isString()) {
      return this->lowering.builder_.CreateSelect(
          this->lowering.optionalLowerer().optionalHas(value, conv->operand->type),
          this->lowering.optionalLowerer().optionalValue(value, conv->operand->type),
          this->lowering.stringLowerer().stringLiteral(u"undefined"));
    }
    return this->lowering.optionalLowerer().optionalValue(value, conv->operand->type);
  }
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                             conv->type->isObject() &&
                                             conv->operand->type->nonOptional()->isObject()) {
    llvm::Value *value = this->emitBorrowed(conv->operand);
    if (value && conv->operand->type->isOptional()) {
      value = this->lowering.optionalLowerer().optionalValue(value, conv->operand->type);
    }
    return value;
  }
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && !conv->shortCircuit &&
      UnionLowerer::narrowsUnion(conv->operand->type, conv->type)) {
    llvm::Value *value = this->emitBorrowed(conv->operand);
    return value
               ? this->lowering.exprLowerer().emitConversion(value, conv->operand->type, conv->type)
               : nullptr;
  }
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && conv->type->isResult() && conv->operand->type->isResult()) {
    llvm::Value *value = this->emitBorrowed(conv->operand);
    return value
               ? this->lowering.exprLowerer().emitConversion(value, conv->operand->type, conv->type)
               : nullptr;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || !member->object->type->isObject() || member->object->type->isBytes() ||
      member->method()) {
    return nullptr;
  }
  if (const Lowering::InterfaceMember *method = this->lowering.inheritLowerer().dispatchedMember(
          member->object->type, member->name, member->signature());
      method && method->callable) {
    return nullptr;
  }
  llvm::Value *object = this->emitBorrowed(member->object);
  if (!object) {
    return nullptr;
  }

  return this->lowering.builder_.CreateLoad(
      this->lowering.moduleBuilder().lower(member->type),
      this->lowering.objectLayouts().fieldAddress(object, member->object->type, member->name),
      member->name);
}

void RefCounts::dropOperand(const Lowering::Operand &operand) {
  if (operand.owned) {
    this->releaseValue(operand.value, operand.type);
  }
}

}
