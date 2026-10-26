#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include <string>
#include <vector>

namespace tz {

class Diagnostics {
public:
  explicit Diagnostics(llvm::SourceMgr &sourceMgr) : sourceMgr_(sourceMgr) {}

  void error(llvm::SMLoc loc, const llvm::Twine &message);
  void note(llvm::SMLoc loc, const llvm::Twine &message);
  void warning(llvm::SMLoc loc, const llvm::Twine &message);
  void tool(const llvm::Twine &message);

  bool setMuted(bool muted) {
    bool old = muted_;
    muted_ = muted;

    return old;
  }

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

  Errors take();
  void quietRange(const char *begin, const char *end) { quiet_.push_back({begin, end}); }

  llvm::SourceMgr &sourceMgr() const { return sourceMgr_; }

  bool hasErrors() const { return errorCount_ != 0; }
  unsigned errorCount() const { return errorCount_; }
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

}
