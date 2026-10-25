#pragma once

#include "llvm/ADT/Twine.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"

#include <string>
#include <vector>

namespace tz {

// Reports compiler diagnostics as `file:line:col: error: message`, followed by
// the offending source line and a caret under the location.
class Diagnostics {
public:
  explicit Diagnostics(llvm::SourceMgr &sourceMgr) : sourceMgr_(sourceMgr) {}

  void error(llvm::SMLoc loc, const llvm::Twine &message);
  void note(llvm::SMLoc loc, const llvm::Twine &message);
  // `file:line:col: warning: message`: something that is likely wrong but
  // compiles (see checkIntOverflow); it does not stop the build.
  void warning(llvm::SMLoc loc, const llvm::Twine &message);

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

  // While recording, what would be printed is kept instead, in order, until
  // replay() prints it: the driver checks a program in rounds when its event
  // maps come from its emit() calls (see EventInference in sema.h), and
  // shows only what the last round says.
  void setRecording(bool recording) { recording_ = recording; }
  void replay();
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
  struct Recorded {
    llvm::SourceMgr::DiagKind kind;
    llvm::SMLoc loc;
    std::string message;
  };
  bool recording_ = false;
  std::vector<Recorded> recorded_;
  std::vector<std::pair<const char *, const char *>> quiet_;
};

} // namespace tz
