#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// tz build --explain-parallel: the decisions in words.
class Explainer {
public:
  explicit Explainer(Analysis &analysis);

  void print(llvm::raw_ostream &out);
  Analysis::Pos posOf(llvm::SMLoc loc) const;
  std::string where(llvm::SMLoc loc) const;
  unsigned lineOf(llvm::SMLoc loc) const;
  std::string fnTitle(const FnInfo &f) const;
  std::string fnName(const FnInfo &f) const;
  std::string segTitle(const FnInfo &f, unsigned seg) const;
  std::string rootTitle(int root) const;
  std::string rootTitleAt(int root, bool column) const;
  std::string pieceTitle(unsigned piece, bool withContext) const;
  std::string contextOf(unsigned piece) const;
  std::string refName(Ref r) const;
  std::string locName(const Loc &loc) const;
  std::string typeText(const Type *t) const;
  std::string calleeText(const Expr *e) const;
  std::string callText(const CallExpr *c) const;
  std::string positions(const std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> &ats) const;
  std::vector<std::string> decisionLines(unsigned piece) const;

private:
  Analysis &analysis_;
};

} // namespace tz::parallel
