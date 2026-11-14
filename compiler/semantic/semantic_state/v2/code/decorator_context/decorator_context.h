#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"

namespace tz {

struct DescriptorOrigin {
  const Type *args = nullptr;
  const Type *ret = nullptr;
};

class DecoratorContext {
public:
  void rememberDescriptorOrigin(const Type *type, const Type *args, const Type *returnType);


  const DescriptorOrigin *findDescriptorOrigin(const Type *type) const;

private:
  llvm::DenseMap<const Type *, DescriptorOrigin> descriptorOrigins;
};

}
