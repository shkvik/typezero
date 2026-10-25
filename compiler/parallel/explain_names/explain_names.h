#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// How --explain-parallel names what it shows: places, types, calls, references and
// locations.
class ExplainNames {
public:
  explicit ExplainNames(Analysis &analysis);

  Analysis::Pos posOf(llvm::SMLoc loc) const;
  std::string where(llvm::SMLoc loc) const;
  unsigned lineOf(llvm::SMLoc loc) const;
  std::string refName(Ref r) const;
  std::string locName(const Loc &loc) const;
  std::string typeText(const Type *t) const;
  std::string calleeText(const Expr *e) const;
  std::string callText(const CallExpr *c) const;
  std::string positions(const std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> &ats) const;

private:
  Analysis &analysis_;
};

} // namespace tz::parallel
