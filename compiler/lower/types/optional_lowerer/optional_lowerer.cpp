#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"

using llvm::dyn_cast;

namespace tz {

OptionalLowerer::OptionalLowerer(Lowering &owner)
    : lowering(owner) {}

bool OptionalLowerer::isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

llvm::Value *OptionalLowerer::makeOptional(llvm::Value *value, llvm::Value *has, const Type *type) {
  if (type->isUnknown() || UnionLayout::zeroIsUndefined(type->inner())) {
    auto *always = dyn_cast<llvm::ConstantInt>(has);
    if (always && always->isOne()) {
      return value;
    }
    return this->lowering.builder.CreateSelect(has, value,
                                               llvm::Constant::getNullValue(value->getType()));
  }
  llvm::Value *result = llvm::UndefValue::get(this->lowering.moduleBuilder().lower(type));
  result = this->lowering.builder.CreateInsertValue(result, value, 0);

  return this->lowering.builder.CreateInsertValue(result, has, 1);
}

llvm::Value *OptionalLowerer::undefinedValue(const Type *optionalType) {
  return llvm::Constant::getNullValue(this->lowering.moduleBuilder().lower(optionalType));
}

llvm::Value *OptionalLowerer::optionalHas(llvm::Value *value, const Type *optionalType) {
  if (optionalType->isUnknown()) {
    return this->lowering.builder.CreateIsNotNull(value);
  }
  const Type *inner = optionalType->inner();
  if (OptionalLowerer::isReference(inner)) {
    return this->lowering.builder.CreateIsNotNull(value);
  }
  if (UnionLayout::isAtomType(inner)) {
    return this->lowering.builder.CreateICmpNE(value, this->lowering.builder.getInt32(0));
  }
  if (inner->isTagged()) {
    return this->lowering.builder.CreateICmpNE(this->lowering.unionLayout().unionTag(value),
                                               this->lowering.builder.getInt32(0));
  }

  return this->lowering.builder.CreateExtractValue(value, 1);
}

llvm::Value *OptionalLowerer::optionalValue(llvm::Value *value, const Type *optionalType) {
  if (optionalType->isUnknown() || UnionLayout::zeroIsUndefined(optionalType->inner())) {
    return value;
  }

  return this->lowering.builder.CreateExtractValue(value, 0);
}

}
