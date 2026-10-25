#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// Type descriptor kinds; must match runtime/core/tzrt/tzrt.h.
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
  case Type::Kind::Void:  // the half of `ok()`, which holds undefined
  case Type::Kind::Never: // a half that holds nothing
    return 8;
  case Type::Kind::Function:
    return 9;
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::ClassRef:
    return 10; // an object of the runtime, which prints as one
  case Type::Kind::Decimal:
    return 11;
  case Type::Kind::Result:
    return 12;
  case Type::Kind::Json:
    return 13;
  case Type::Kind::Unknown:
    return 16; // TZ_T_UNKNOWN
  default:
    llvm_unreachable("no runtime descriptor for this type");
  }
}

} // namespace

TypeDescriptors::TypeDescriptors(CodeGen &codegen) : codegen_(codegen) {}

// A constant { i32 kind, i32 size, ptr inner } describing a type to the
// runtime: how big its values are and how to release, compare and print them.
llvm::Constant *TypeDescriptors::typeDescriptor(const Type *type) {
  if (llvm::Constant *found = codegen_.typeDescriptors_.lookup(type)) {
    return found;
  }
  if (type->isResult()) {
    return codegen_.resultLowerer().resultDescriptor(type);
  }
  if (type->isUnion() || type->isLiteral()) {
    return codegen_.unionLayout().unionDescriptor(type);
  }
  // The element of an array, the value of `T | undefined`; a function's
  // inner() is its return type, which is not part of the value.
  llvm::Constant *inner = type->isArray() || type->isOptional()
                              ? typeDescriptor(type->inner())
                              : llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(codegen_.resultLowerer().lowerHalf(type));
  llvm::Constant *init = llvm::ConstantStruct::get(
      codegen_.typeDescTy_, {codegen_.builder_.getInt32(descriptorKind(type)),
                    codegen_.builder_.getInt32(static_cast<uint32_t>(size)), inner});
  auto *global =
      new llvm::GlobalVariable(*codegen_.module_, codegen_.typeDescTy_, /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, init, "tz.type." + type->name());
  // Reflection tells the types of functions and arrays in unknowns apart by
  // their descriptors (runtime/library/reflect/reflect/reflect.c), which must
  // not be merged.
  if (!type->isFunction() && !type->isArray()) {
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  }
  codegen_.typeDescriptors_[type] = global;

  return global;
}

} // namespace tz
