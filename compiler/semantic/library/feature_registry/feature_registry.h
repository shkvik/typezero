#pragma once

#include "compiler/semantic/feature/feature.h"
#include <memory>
#include <vector>

namespace tz {

class FeatureRegistry {
public:
  FeatureRegistry();

  llvm::ArrayRef<const Feature *> all() const {
    return this->order;
  }

private:
  std::vector<std::unique_ptr<Feature>> features;
  std::vector<const Feature *> order;
};

}
