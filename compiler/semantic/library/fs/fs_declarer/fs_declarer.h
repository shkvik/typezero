#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FsDeclarer {
public:
  explicit FsDeclarer(SemanticState &owner);

  void declareFsClasses();
  Expr *checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkStatsDate(MemberExpr *member, ClassDecl *cls);
  bool isRuntimeClass(const ClassDecl *cls) const;

private:
  static std::optional<int> fileKind(llvm::StringRef method);

  SemanticState &state;
};

}
