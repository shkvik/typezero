#include "compiler/semantic/semantic_state/v2/code/decorator_context/decorator_context.h"

namespace tz {

void DecoratorContext::rememberDescriptorOrigin(const Type *type, const Type *args,
                                                const Type *returnType) {
  this->descriptorOrigins.try_emplace(type, DescriptorOrigin{args, returnType});
}

const DescriptorOrigin *DecoratorContext::findDescriptorOrigin(const Type *type) const {
  auto found = this->descriptorOrigins.find(type);
  if (found == this->descriptorOrigins.end()) {
    return nullptr;
  }

  return &found->second;
}

}
