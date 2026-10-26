#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FsDeclarer {
public:
  explicit FsDeclarer(Checker &checker);

  void declareFsClasses();
  Expr *checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkStatsDate(MemberExpr *member, ClassDecl *cls);
  bool isRuntimeClass(const ClassDecl *cls) const;

private:
  Checker &checker_;
};

}
