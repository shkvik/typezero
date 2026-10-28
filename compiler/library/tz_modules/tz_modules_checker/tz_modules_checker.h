#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TzModulesChecker {
public:
  explicit TzModulesChecker(Checker &checker);

  Expr *adaptResult(Expr *expr, const Type *to);
  void checkExternal(FunctionDecl *fn);
  void noteHttpsCall(CallExpr *call, const FunctionDecl *fn);
  void noteListen(CallExpr *call, const MemberExpr *member);
  void noteServeOptions(const Expr *options);
  void noteHttpsValue(const FunctionDecl *fn, llvm::SMLoc loc);
  std::string knownPrefix(const Expr *expr) const;

private:
  Checker &checker_;
};

}
