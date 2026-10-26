#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class FsFeature : public Feature {
public:
  llvm::StringRef name() const override { return "fs"; }

  void declare(Checker &checker) const override;
};

}
