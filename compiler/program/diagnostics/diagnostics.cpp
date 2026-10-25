#include "compiler/program/diagnostics/diagnostics.h"

namespace tz {

void Diagnostics::error(llvm::SMLoc loc, const llvm::Twine &message) {
  ++errorsSeen_;
  if (muted_) {
    return;
  }
  if (capture_) {
    capture_->push_back({loc, message.str()});
    return;
  }
  ++errorCount_;
  errors_.add({llvm::SourceMgr::DK_Error, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::warning(llvm::SMLoc loc, const llvm::Twine &message) {
  if (muted_ || capture_) {
    return;
  }
  for (const auto &[begin, end] : quiet_) {
    if (loc.getPointer() >= begin && loc.getPointer() < end) {
      return;
    }
  }
  errors_.add({llvm::SourceMgr::DK_Warning, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::note(llvm::SMLoc loc, const llvm::Twine &message) {
  if (muted_ || capture_) {
    return;
  }
  errors_.add({llvm::SourceMgr::DK_Note, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::tool(const llvm::Twine &message) {
  ++errorsSeen_;
  ++errorCount_;
  errors_.tool(message);
}

Errors Diagnostics::take() {
  Errors taken = std::move(errors_);
  errors_ = Errors();

  return taken;
}

} // namespace tz
