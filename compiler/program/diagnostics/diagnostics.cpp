#include "compiler/program/diagnostics/diagnostics.h"

#include "llvm/Support/raw_ostream.h"

namespace tz {

void Diagnostics::error(llvm::SMLoc loc, const llvm::Twine &message) {
  ++errorsSeen_;
  if (muted_)
    return;
  if (capture_) {
    capture_->push_back({loc, message.str()});
    return;
  }
  ++errorCount_;
  if (recording_) {
    recorded_.push_back({llvm::SourceMgr::DK_Error, loc, message.str()});
    return;
  }
  sourceMgr_.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Error, message);
}

void Diagnostics::warning(llvm::SMLoc loc, const llvm::Twine &message) {
  if (muted_ || capture_)
    return;
  for (const auto &[begin, end] : quiet_)
    if (loc.getPointer() >= begin && loc.getPointer() < end)
      return;
  // In the rounds of event inference, only the last round's are shown.
  if (recording_) {
    recorded_.push_back({llvm::SourceMgr::DK_Warning, loc, message.str()});
    return;
  }
  sourceMgr_.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Warning, message);
}

void Diagnostics::note(llvm::SMLoc loc, const llvm::Twine &message) {
  if (muted_ || capture_)
    return;
  if (recording_) {
    recorded_.push_back({llvm::SourceMgr::DK_Note, loc, message.str()});
    return;
  }
  sourceMgr_.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Note, message);
}

void Diagnostics::replay() {
  for (const Recorded &r : recorded_)
    sourceMgr_.PrintMessage(llvm::errs(), r.loc, r.kind, r.message);
  recorded_.clear();
}

} // namespace tz
