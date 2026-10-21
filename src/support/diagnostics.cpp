#include "support/Diagnostics.h"

#include "llvm/Support/raw_ostream.h"

namespace tz {

void Diagnostics::error(llvm::SMLoc loc, const llvm::Twine &message) {
  ++errorCount_;
  sourceMgr_.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Error, message);
}

void Diagnostics::note(llvm::SMLoc loc, const llvm::Twine &message) {
  sourceMgr_.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Note, message);
}

} // namespace tz
