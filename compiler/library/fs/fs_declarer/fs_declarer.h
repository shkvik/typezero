#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FsDeclarer {
public:
  explicit FsDeclarer(Checker &owner);

  void declareFsClasses();
  Expr *checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkStatsDate(MemberExpr *member, ClassDecl *cls);
  bool isRuntimeClass(const ClassDecl *cls) const;

private:
  static std::optional<int> fileKind(llvm::StringRef method);

  Checker &checker;
};

}
