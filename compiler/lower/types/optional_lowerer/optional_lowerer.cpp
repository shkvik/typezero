#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_layout/union_layout.h"
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

namespace {

bool isReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isJson() || type->isDict() ||
         type->isPromise() || type->isUnknown() || type->isClassRef() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

}

OptionalLowerer::OptionalLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *OptionalLowerer::makeOptional(llvm::Value *value, llvm::Value *has, const Type *type) {
  if (type->isUnknown() || UnionLayout::zeroIsUndefined(type->inner())) {
    if (auto *always = dyn_cast<llvm::ConstantInt>(has); always && always->isOne()) {
      return value;
    }
    return lowering_.builder_.CreateSelect(has, value,
                                           llvm::Constant::getNullValue(value->getType()));
  }
  llvm::Value *result = llvm::UndefValue::get(lowering_.moduleBuilder().lower(type));
  result = lowering_.builder_.CreateInsertValue(result, value, 0);

  return lowering_.builder_.CreateInsertValue(result, has, 1);
}

llvm::Value *OptionalLowerer::undefinedValue(const Type *optionalType) {
  return llvm::Constant::getNullValue(lowering_.moduleBuilder().lower(optionalType));
}

llvm::Value *OptionalLowerer::optionalHas(llvm::Value *value, const Type *optionalType) {
  if (optionalType->isUnknown()) {
    return lowering_.builder_.CreateIsNotNull(value);
  }
  const Type *inner = optionalType->inner();
  if (isReference(inner)) {
    return lowering_.builder_.CreateIsNotNull(value);
  }
  if (UnionLayout::isAtomType(inner)) {
    return lowering_.builder_.CreateICmpNE(value, lowering_.builder_.getInt32(0));
  }
  if (inner->isTagged()) {
    return lowering_.builder_.CreateICmpNE(lowering_.unionLayout().unionTag(value),
                                           lowering_.builder_.getInt32(0));
  }

  return lowering_.builder_.CreateExtractValue(value, 1);
}

llvm::Value *OptionalLowerer::optionalValue(llvm::Value *value, const Type *optionalType) {
  if (optionalType->isUnknown() || UnionLayout::zeroIsUndefined(optionalType->inner())) {
    return value;
  }

  return lowering_.builder_.CreateExtractValue(value, 0);
}

}
