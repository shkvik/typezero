#include "compiler/semantic/semantic_state/v2/flow/assigned_field_context/assigned_field_context.h"

namespace tz {

void AssignedFieldContext::markFieldAssigned(llvm::StringRef name) {
  this->assignedFields.insert(name);
}

bool AssignedFieldContext::isFieldAssigned(llvm::StringRef name) const {
  return this->assignedFields.contains(name);
}

}
