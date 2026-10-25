#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// node:fs, node:fs/promises and node:readline (docs/io.md): the classes they make.
class FsFeature : public Feature {
public:
  llvm::StringRef name() const override { return "fs"; }

  void declare(Sema &sema) const override;
};

} // namespace tz
