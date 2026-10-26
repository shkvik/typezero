#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class ExplainTitles {
public:
  explicit ExplainTitles(Analysis &analysis);

  std::string fnTitle(const FnInfo &f) const;
  std::string fnName(const FnInfo &f) const;
  std::string segTitle(const FnInfo &f, unsigned seg) const;
  std::string rootTitle(int root) const;
  std::string rootTitleAt(int root, bool column) const;
  std::string pieceTitle(unsigned piece, bool withContext) const;
  std::string contextOf(unsigned piece) const;

private:
  Analysis &analysis_;
};

}
