#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/Support/SourceMgr.h"

namespace tz {

// Prints what the passes found on stderr: `file:line:col: error: message` with the line and a
// caret under the place, or `tz: message` for an error of tz itself.
class ErrorPrinter {
public:
  explicit ErrorPrinter(const llvm::SourceMgr &sourceMgr);

  void print(const Errors &errors) const;

private:
  const llvm::SourceMgr &sourceMgr_;
};

} // namespace tz
