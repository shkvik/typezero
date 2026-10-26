#pragma once

#include "compiler/library/feature/feature.h"
#include <memory>
#include <vector>

namespace tz {

class FeatureRegistry {
public:
  static const FeatureRegistry &standard();

  llvm::ArrayRef<const Feature *> all() const { return order_; }
  const Feature *lowering(Builtin builtin) const;

private:
  FeatureRegistry();

  std::vector<std::unique_ptr<Feature>> features_;
  std::vector<const Feature *> order_;
};

}
