#include "compiler/semantic/semantic_state/v2/code/argument_list_context/argument_list_context.h"

namespace tz {

void ArgumentListContext::rememberArgumentList(const VarSymbol *variable,
                                               std::vector<llvm::StringRef> parts) {
  this->argumentLists[variable] = std::move(parts);
}

std::optional<std::vector<llvm::StringRef>>
ArgumentListContext::findArgumentList(const VarSymbol *variable) const {
  auto found = this->argumentLists.find(variable);
  if (found == this->argumentLists.end()) {
    return std::nullopt;
  }

  return found->second;
}

}
