#include "compiler/semantic/semantic_state/v2/generics/type_parameter_context/type_parameter_context.h"

namespace tz {

void TypeParameterContext::rememberExactParams(const FuncSymbol *generic, std::vector<bool> exact) {
  this->exactParams[generic] = std::move(exact);
}

std::optional<std::vector<bool>>
TypeParameterContext::findExactParams(const FuncSymbol *generic) const {
  auto found = this->exactParams.find(generic);
  if (found == this->exactParams.end()) {
    return std::nullopt;
  }

  return found->second;
}

void TypeParameterContext::rememberNumberParam(const Type *param, const Type *only,
                                               const Type *number) {
  this->numberParams[{param, only}] = number;
}

const Type *TypeParameterContext::findNumberParam(const Type *param, const Type *only) const {
  return this->numberParams.lookup({param, only});
}

void TypeParameterContext::enterSubstitution(const Type *type) {
  this->substituting.insert(type);
}

void TypeParameterContext::leaveSubstitution(const Type *type) {
  this->substituting.erase(type);
}

bool TypeParameterContext::isSubstituting(const Type *type) const {
  return this->substituting.contains(type);
}

}
