#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/types/union_layout/union_layout.h"
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

OptionalLowerer::OptionalLowerer(CodeGen &codegen) : codegen_(codegen) {}

// An unknown holds undefined itself: it is its own `T | undefined`, null for
// undefined.
llvm::Value *OptionalLowerer::makeOptional(llvm::Value *value, llvm::Value *has, const Type *type) {
  if (type->isUnknown() || UnionLayout::zeroIsUndefined(type->inner())) {
    if (auto *always = dyn_cast<llvm::ConstantInt>(has); always && always->isOne()) {
      return value;
    }
    return codegen_.builder_.CreateSelect(has, value, llvm::Constant::getNullValue(value->getType()));
  }
  llvm::Value *result = llvm::UndefValue::get(codegen_.moduleBuilder().lower(type));
  result = codegen_.builder_.CreateInsertValue(result, value, 0);

  return codegen_.builder_.CreateInsertValue(result, has, 1);
}

llvm::Value *OptionalLowerer::undefinedValue(const Type *optionalType) {
  return llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(optionalType));
}

llvm::Value *OptionalLowerer::optionalHas(llvm::Value *value, const Type *optionalType) {
  if (optionalType->isUnknown()) {
    return codegen_.builder_.CreateIsNotNull(value);
  }
  const Type *inner = optionalType->inner();
  if (isReference(inner)) {
    return codegen_.builder_.CreateIsNotNull(value);
  }
  if (UnionLayout::isAtomType(inner)) { // atoms start at 1
    return codegen_.builder_.CreateICmpNE(value, codegen_.builder_.getInt32(0));
  }
  if (inner->isTagged()) { // tags too
    return codegen_.builder_.CreateICmpNE(codegen_.unionLayout().unionTag(value), codegen_.builder_.getInt32(0));
  }

  return codegen_.builder_.CreateExtractValue(value, 1);
}

llvm::Value *OptionalLowerer::optionalValue(llvm::Value *value, const Type *optionalType) {
  if (optionalType->isUnknown() || UnionLayout::zeroIsUndefined(optionalType->inner())) {
    return value;
  }

  return codegen_.builder_.CreateExtractValue(value, 0);
}

} // namespace tz
