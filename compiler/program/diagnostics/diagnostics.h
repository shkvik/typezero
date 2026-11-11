#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include <string>
#include <utility>
#include <vector>

namespace tz {

enum class DiagnosticRoute {
  PARENT,
  PROGRAM,
  DISCARD,
  OWNER,
};

class Diagnostics {
public:
  using ResultTaken = Result<void, std::vector<Diagnostic>>;

  explicit Diagnostics(llvm::SourceMgr &sourceMgr);

  void error(llvm::SMLoc loc, const llvm::Twine &message);
  void errorOutsideTrials(llvm::SMLoc loc, const llvm::Twine &message);
  void note(llvm::SMLoc loc, const llvm::Twine &message);
  void warning(llvm::SMLoc loc, const llvm::Twine &message);
  void hint(llvm::SMLoc loc, const llvm::Twine &message);
  void tool(const llvm::Twine &message);

  void pushList(DiagnosticRoute route);
  std::vector<Diagnostic> popList();
  DiagnosticRoute getCurrentDestination() const;
  const Diagnostic *findFirstError(const std::vector<Diagnostic> &found) const;
  unsigned getWrittenErrorCount() const;

  ResultTaken take();
  static bool hasErrors(const std::vector<Diagnostic> &found);
  void quietRange(const char *begin, const char *end) {
    this->quiet.push_back({begin, end});
  }
  bool isQuiet(llvm::SMLoc loc) const;

  llvm::SourceMgr &sourceMgr() const { return sourceMgr_; }

private:
  struct DiagnosticList {
    DiagnosticRoute route;
    std::vector<Diagnostic> found;
  };

  DiagnosticRoute destinationOf(size_t index) const;

  llvm::SourceMgr &sourceMgr_;
  unsigned writtenErrors = 0;
  std::vector<DiagnosticList> lists;
  std::vector<std::pair<const char *, const char *>> quiet;
};

}
