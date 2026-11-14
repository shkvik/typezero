#include "compiler/semantic/semantic_state/v2/flow/fixed_length_context/fixed_length_context.h"

namespace tz {

void FixedLengthContext::rememberFixedLength(const VarSymbol *variable, int64_t length) {
  this->fixedLengths[variable] = length;
}

std::optional<int64_t> FixedLengthContext::findFixedLength(const VarSymbol *variable) const {
  auto found = this->fixedLengths.find(variable);
  if (found == this->fixedLengths.end()) {
    return std::nullopt;
  }

  return found->second;
}

}
