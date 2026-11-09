#include "compiler/semantic/flow/tree_place/tree_place.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

bool TreePlace::isNested() const {
  return this->isInsideLoop() || this->isInsideFunction();
}

bool TreePlace::isInsideLoop() const {
  return this->loops > 0;
}

bool TreePlace::isInsideSwitch() const {
  return this->switches > 0;
}

bool TreePlace::isInsideFunction() const {
  return this->functions > 0;
}

bool TreePlace::hasLabel(llvm::StringRef label) const {
  return llvm::is_contained(this->labels, label);
}

TreePlace TreePlace::enterLoop() const {
  TreePlace inside = *this;
  inside.loops += 1;

  return inside;
}

TreePlace TreePlace::enterSwitch() const {
  TreePlace inside = *this;
  inside.switches += 1;

  return inside;
}

TreePlace TreePlace::enterFunction() const {
  TreePlace inside = *this;
  inside.functions += 1;

  return inside;
}

TreePlace TreePlace::enterLabel(llvm::StringRef label) const {
  TreePlace inside = *this;
  inside.labels.push_back(label);

  return inside;
}

}
