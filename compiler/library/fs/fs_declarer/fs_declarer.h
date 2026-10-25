#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Stats, Dirent and SystemError, and their methods.
class FsDeclarer {
public:
  explicit FsDeclarer(Checker &checker);

  // The classes Stats and Dirent of node:fs.
  void declareFsClasses();
  // isFile() and the like of Stats and Dirent; null for other methods.
  Expr *checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkStatsDate(MemberExpr *member, ClassDecl *cls);
  // Stats, Dirent and SystemError: objects only the runtime makes.
  bool isRuntimeClass(const ClassDecl *cls) const;

private:
  Checker &checker_;
};

} // namespace tz
