#include "compiler/semantic/semantic_state/v2/code/constructor_context/constructor_context.h"

namespace tz {

void ConstructorContext::markRootSuper(const CallExpr *call) {
  this->rootSupers.insert(call);
}

bool ConstructorContext::isRootSuper(const CallExpr *call) const {
  return this->rootSupers.contains(call);
}

void ConstructorContext::markCallReported(const CallExpr *call) {
  this->reportedCalls.insert(call);
}

bool ConstructorContext::isCallReported(const CallExpr *call) const {
  return this->reportedCalls.contains(call);
}

}
