#include "compiler/program/diagnostics/diagnostics.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

Diagnostics::Diagnostics(llvm::SourceMgr &sourceMgr)
    : sourceMgr_(sourceMgr) {
  this->lists.push_back(DiagnosticList{.route = DiagnosticRoute::PROGRAM, .found = {}});
}

void Diagnostics::error(llvm::SMLoc loc, const llvm::Twine &message) {
  ++this->writtenErrors;
  this->lists.back().found.push_back(
      {llvm::SourceMgr::DK_Error, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::errorOutsideTrials(llvm::SMLoc loc, const llvm::Twine &message) {
  ++this->writtenErrors;
  size_t at = this->lists.size() - 1;
  while (at > 0 && this->destinationOf(at) == DiagnosticRoute::DISCARD) {
    --at;
  }
  this->lists[at].found.push_back(
      {llvm::SourceMgr::DK_Error, Diagnostic::Form::Located, loc, message.str()});
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
  if (this->isQuiet(loc)) {
    return;
  }
  this->lists.back().found.push_back(
      {llvm::SourceMgr::DK_Warning, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::hint(llvm::SMLoc loc, const llvm::Twine &message) {
  if (this->isQuiet(loc)) {
    return;
  }
  this->lists.back().found.push_back(
      {llvm::SourceMgr::DK_Remark, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::note(llvm::SMLoc loc, const llvm::Twine &message) {
  this->lists.back().found.push_back(
      {llvm::SourceMgr::DK_Note, Diagnostic::Form::Located, loc, message.str()});
}

void Diagnostics::tool(const llvm::Twine &message) {
  ++this->writtenErrors;
  this->lists.front().found.push_back({.form = Diagnostic::Form::Tool, .message = message.str()});
}

void Diagnostics::pushList(DiagnosticRoute route) {
  this->lists.push_back(DiagnosticList{.route = route, .found = {}});
}

std::vector<Diagnostic> Diagnostics::popList() {
  DiagnosticList top = std::move(this->lists.back());
  this->lists.pop_back();
  if (top.route == DiagnosticRoute::PARENT) {
    llvm::append_range(this->lists.back().found, top.found);
  }
  if (top.route == DiagnosticRoute::PROGRAM) {
    llvm::append_range(this->lists.front().found, top.found);
  }

  return top.found;
}

DiagnosticRoute Diagnostics::getCurrentDestination() const {
  return this->destinationOf(this->lists.size() - 1);
}

DiagnosticRoute Diagnostics::destinationOf(size_t index) const {
  size_t at = index;
  while (at > 0 && this->lists[at].route == DiagnosticRoute::PARENT) {
    --at;
  }

  return this->lists[at].route;
}

const Diagnostic *Diagnostics::findFirstError(const std::vector<Diagnostic> &found) const {
  for (const Diagnostic &diagnostic : found) {
    if (diagnostic.kind == llvm::SourceMgr::DK_Error) {
      return &diagnostic;
    }
  }

  return nullptr;
}

unsigned Diagnostics::getWrittenErrorCount() const {
  return this->writtenErrors;
}

Diagnostics::ResultTaken Diagnostics::take() {
  std::vector<Diagnostic> taken = std::move(this->lists.front().found);
  this->lists.front().found.clear();
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
