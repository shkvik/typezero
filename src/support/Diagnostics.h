#pragma once

#include "llvm/ADT/Twine.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

namespace tz {

// Reports compiler diagnostics as `file:line:col: error: message`, followed by
// the offending source line and a caret under the location.
class Diagnostics {
public:
  explicit Diagnostics(llvm::SourceMgr &sourceMgr) : sourceMgr_(sourceMgr) {}

  void error(llvm::SMLoc loc, const llvm::Twine &message);
  void note(llvm::SMLoc loc, const llvm::Twine &message);

  bool hasErrors() const { return errorCount_ != 0; }
  unsigned errorCount() const { return errorCount_; }

private:
  llvm::SourceMgr &sourceMgr_;
  unsigned errorCount_ = 0;
};

} // namespace tz
