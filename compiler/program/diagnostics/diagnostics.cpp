#include "compiler/program/diagnostics/diagnostics.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

void Diagnostics::error(llvm::SMLoc loc, const llvm::Twine &message) {
  ++errorsSeen_;
  if (this->muted) {
    return;
  }
  if (capture_) {
    capture_->push_back({loc, message.str()});
    return;
  }
  ++errorCount_;
  this->found.push_back({llvm::SourceMgr::DK_Error, Diagnostic::Form::Located, loc, message.str()});
}

bool Diagnostics::isQuiet(llvm::SMLoc loc) const {
  for (const auto &[begin, end] : this->quiet) {
    if (loc.getPointer() >= begin && loc.getPointer() < end) {
      return true;
    }
  }

  return false;
}

void Diagnostics::warning(llvm::SMLoc loc, const llvm::Twine &message) {
  if (this->muted || capture_ || isQuiet(loc)) {
    return;
  }
  this->found.push_back(
      {llvm::SourceMgr::DK_Warning, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::hint(llvm::SMLoc loc, const llvm::Twine &message) {
  if (this->muted || capture_ || isQuiet(loc)) {
    return;
  }
  this->found.push_back(
      {llvm::SourceMgr::DK_Remark, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::note(llvm::SMLoc loc, const llvm::Twine &message) {
  if (this->muted || capture_) {
    return;
  }
  this->found.push_back({llvm::SourceMgr::DK_Note, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::tool(const llvm::Twine &message) {
  ++errorsSeen_;
  ++errorCount_;
  this->found.push_back({.form = Diagnostic::Form::Tool, .message = message.str()});
}

Diagnostics::ResultTaken Diagnostics::take() {
  std::vector<Diagnostic> taken = std::move(this->found);
  this->found.clear();
  if (!taken.empty()) {
    return err(taken);
  }

  return ok();
}

bool Diagnostics::hasErrors(const std::vector<Diagnostic> &found) {
  for (const Diagnostic &diagnostic : found) {
    if (diagnostic.kind == llvm::SourceMgr::DK_Error) {
      return true;
    }
  }

  return false;
}

}
