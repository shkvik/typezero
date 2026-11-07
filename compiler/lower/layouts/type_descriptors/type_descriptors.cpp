#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"


namespace tz {

uint32_t TypeDescriptors::descriptorKind(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Int:
    switch (type->bitWidth()) {
    case 8:
      return type->isUnsigned() ? 20 : 18;
    case 16:
      return type->isUnsigned() ? 21 : 19;
    case 32:
      return type->isUnsigned() ? 22 : 1;
    default:
      return type->isUnsigned() ? 23 : 2;
    }
  case Type::Kind::Double:
    return 3;
  case Type::Kind::Bool:
    return 4;
  case Type::Kind::String:
    return 5;
  case Type::Kind::Array:
    return 6;
  case Type::Kind::Optional:
    return 7;
  case Type::Kind::Undefined:
  case Type::Kind::Void:
  case Type::Kind::Never:
    return 8;
  case Type::Kind::Function:
    return 9;
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::ClassRef:
    return 10;
  case Type::Kind::Decimal:
    return 11;
  case Type::Kind::Result:
    return 12;
  case Type::Kind::Json:
    return 13;
  case Type::Kind::Unknown:
    return 16;
  default:
    llvm_unreachable("no runtime descriptor for this type");
  }
}

TypeDescriptors::TypeDescriptors(Lowering &owner)
    : lowering(owner) {}

llvm::Constant *TypeDescriptors::typeDescriptor(const Type *type) {
  llvm::Constant *found = this->lowering.typeDescriptors_.lookup(type);
  if (found) {
    return found;
  }
  if (type->isResult()) {
    return this->lowering.resultLowerer().resultDescriptor(type);
  }
  if (type->isUnion() || type->isLiteral()) {
    return this->lowering.unionLayout().unionDescriptor(type);
  }
  llvm::Constant *inner = type->isArray() || type->isOptional()
                              ? this->typeDescriptor(type->inner())
                              : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
  uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(
      this->lowering.resultLowerer().lowerHalf(type));
  llvm::Constant *init = llvm::ConstantStruct::get(
      this->lowering.typeDescTy_,
      {this->lowering.builder_.getInt32(TypeDescriptors::descriptorKind(type)),
       this->lowering.builder_.getInt32(static_cast<uint32_t>(size)), inner});
  auto *global =
      new llvm::GlobalVariable(*this->lowering.module_, this->lowering.typeDescTy_, true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.type." + type->name());
  if (!type->isFunction() && !type->isArray()) {
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  }
  this->lowering.typeDescriptors_[type] = global;

  return global;
}

}
