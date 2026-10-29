#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

class LspCheck {
public:
  explicit LspCheck(const char *argv0);

  int run(llvm::StringRef requestPath, llvm::StringRef responsePath) const;

private:
  const char *argv0_;
};

}
