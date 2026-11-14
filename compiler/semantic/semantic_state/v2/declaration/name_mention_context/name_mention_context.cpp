#include "compiler/semantic/semantic_state/v2/declaration/name_mention_context/name_mention_context.h"

namespace tz {

void NameMentionContext::markUsedInFunction(llvm::StringRef name) {
  this->namesInFunctions.insert(name);
}

bool NameMentionContext::isUsedInFunction(llvm::StringRef name) const {
  return this->namesInFunctions.contains(name);
}

}
