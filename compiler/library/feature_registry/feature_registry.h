#pragma once

#include "compiler/library/feature/feature.h"
#include <memory>
#include <vector>

namespace tz {

// The topics of the library, in the order the passes ask them.
class FeatureRegistry {
public:
  // Every topic the compiler knows.
  static const FeatureRegistry &standard();

  llvm::ArrayRef<const Feature *> all() const { return order_; }
  // The topic whose calls of `builtin` are; null for the calls of the language.
  const Feature *lowering(Builtin builtin) const;

private:
  FeatureRegistry();

  std::vector<std::unique_ptr<Feature>> features_;
  std::vector<const Feature *> order_;
};

} // namespace tz
