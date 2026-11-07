#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class FsDeclarer {
public:
  explicit FsDeclarer(CheckState &owner);

  void declareFsClasses();
  Expr *checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkStatsDate(MemberExpr *member, ClassDecl *cls);
  bool isRuntimeClass(const ClassDecl *cls) const;

private:
  static std::optional<int> fileKind(llvm::StringRef method);

  CheckState &state;
};

}
