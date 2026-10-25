#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include <string>
#include <vector>

namespace tz {

// Collects what a pass reports: errors, warnings and notes at places of the
// program, and the errors of tz itself. The command line prints them
// (cli/error_printer).
class Diagnostics {
public:
  explicit Diagnostics(llvm::SourceMgr &sourceMgr) : sourceMgr_(sourceMgr) {}

  void error(llvm::SMLoc loc, const llvm::Twine &message);
  void note(llvm::SMLoc loc, const llvm::Twine &message);
  // `file:line:col: warning: message`: something that is likely wrong but
  // compiles (see checkIntOverflow); it does not stop the build.
  void warning(llvm::SMLoc loc, const llvm::Twine &message);
  // `tz: message`: an error of tz itself, at no place of the program.
  void tool(const llvm::Twine &message);

  // While muted, diagnostics are dropped: Sema checks copies of code that it
  // checks again for real (see Sema::inferCycle). Returns the old setting.
  bool setMuted(bool muted) {
    bool old = muted_;
    muted_ = muted;

    return old;
  }

  // While capturing, errors are collected instead of printed (and notes
  // dropped): Sema reports an error in a copy of a generic function at the
  // call that needs the copy. Returns the old setting.
  struct Captured {
    llvm::SMLoc loc;
    std::string message;
  };
  std::vector<Captured> *setCapture(std::vector<Captured> *capture) {
    std::vector<Captured> *old = capture_;
    capture_ = capture;

    return old;
  }
  std::vector<Captured> *capture() const { return capture_; }

  // What was reported since the last take, in order.
  Errors take();
  // Warnings in the code of tz's own library (library/) are dropped: they
  // would tell the program's author about code that is not theirs.
  void quietRange(const char *begin, const char *end) { quiet_.push_back({begin, end}); }

  // The files of the program, and those it embeds (see Sema::readEmbedded).
  llvm::SourceMgr &sourceMgr() const { return sourceMgr_; }

  bool hasErrors() const { return errorCount_ != 0; }
  unsigned errorCount() const { return errorCount_; }
  // Every error so far, also those muted or captured: whether something
  // already said what went wrong.
  unsigned errorsSeen() const { return errorsSeen_; }

private:
  llvm::SourceMgr &sourceMgr_;
  unsigned errorCount_ = 0;
  unsigned errorsSeen_ = 0;
  bool muted_ = false;
  std::vector<Captured> *capture_ = nullptr;
  Errors errors_;
  std::vector<std::pair<const char *, const char *>> quiet_;
};

} // namespace tz
