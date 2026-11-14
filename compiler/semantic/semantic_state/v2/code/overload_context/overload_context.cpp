#include "compiler/semantic/semantic_state/v2/code/overload_context/overload_context.h"

namespace tz {

void OverloadContext::rememberSignatureVariants(const Type *interfaceType, llvm::StringRef name,
                                                std::vector<FunctionDecl *> variants) {
  this->signatureVariants[{interfaceType, name.str()}] = std::move(variants);
}

std::vector<FunctionDecl *> OverloadContext::getSignatureVariants(const Type *interfaceType,
                                                                  llvm::StringRef name) const {
  auto found = this->signatureVariants.find({interfaceType, name.str()});
  if (found == this->signatureVariants.end()) {
    return {};
  }

  return found->second;
}

}
