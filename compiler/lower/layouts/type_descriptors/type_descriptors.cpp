#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

uint32_t descriptorKind(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Int:
    return 1;
  case Type::Kind::Long:
    return 2;
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

}

TypeDescriptors::TypeDescriptors(Lowering &lowering) : lowering_(lowering) {}

llvm::Constant *TypeDescriptors::typeDescriptor(const Type *type) {
  if (llvm::Constant *found = lowering_.typeDescriptors_.lookup(type)) {
    return found;
  }
  if (type->isResult()) {
    return lowering_.resultLowerer().resultDescriptor(type);
  }
  if (type->isUnion() || type->isLiteral()) {
    return lowering_.unionLayout().unionDescriptor(type);
  }
  llvm::Constant *inner = type->isArray() || type->isOptional()
                              ? typeDescriptor(type->inner())
                              : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(
      lowering_.resultLowerer().lowerHalf(type));
  llvm::Constant *init = llvm::ConstantStruct::get(
      lowering_.typeDescTy_, {lowering_.builder_.getInt32(descriptorKind(type)),
                              lowering_.builder_.getInt32(static_cast<uint32_t>(size)), inner});
  auto *global =
      new llvm::GlobalVariable(*lowering_.module_, lowering_.typeDescTy_, true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.type." + type->name());
  if (!type->isFunction() && !type->isArray()) {
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  }
  lowering_.typeDescriptors_[type] = global;

  return global;
}

}
