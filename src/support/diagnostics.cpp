#include "support/diagnostics.h"

#include "llvm/Support/raw_ostream.h"

namespace tz {

void Diagnostics::error(llvm::SMLoc loc, const llvm::Twine &message) {
  if (muted_)
    return;
  ++errorCount_;
  sourceMgr_.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Error, message);
}

void Diagnostics::note(llvm::SMLoc loc, const llvm::Twine &message) {
  if (muted_)
    return;
  sourceMgr_.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Note, message);
}

} // namespace tz
