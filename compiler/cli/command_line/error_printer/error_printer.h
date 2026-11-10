#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "llvm/Support/SourceMgr.h"
#include <vector>

namespace tz {

class ErrorPrinter {
public:
  explicit ErrorPrinter(const llvm::SourceMgr &sourceMgr, bool hints = false);

  void print(const std::vector<Diagnostic> &diagnostics) const;

private:
  const llvm::SourceMgr &sourceMgr_;
  bool hints;
};

}
