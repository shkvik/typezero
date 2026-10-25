#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SMLoc.h"
#include <vector>

namespace tz {

class Errors {
public:
  void add(Diagnostic diagnostic);
  void add(const Errors &other);
  void tool(const llvm::Twine &message);
  void raw(const llvm::Twine &text, llvm::SourceMgr::DiagKind kind = llvm::SourceMgr::DK_Error);

  explicit operator bool() const { return errorCount_ != 0; }

  std::vector<Diagnostic>::const_iterator begin() const { return list_.begin(); }
  std::vector<Diagnostic>::const_iterator end() const { return list_.end(); }

private:
  std::vector<Diagnostic> list_;
  unsigned errorCount_ = 0;
};

} // namespace tz
