#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/Support/SMLoc.h"
#include <string>
#include <utility>
#include <vector>

namespace tz::parallel {

class ExplainNames {
public:
  explicit ExplainNames(Analysis &analysis);

  Pos posOf(llvm::SMLoc loc) const;
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

}
