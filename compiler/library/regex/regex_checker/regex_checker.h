#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class RegexChecker {
public:
  explicit RegexChecker(Checker &checker);

  Expr *checkRegexLit(RegexLitExpr *lit);
  static const RegexLitExpr *knownRegex(const Expr *expr);
  const Type *matchType(const RegexLitExpr *regex);
  void noteRegexVariable(VarSymbol *var, const Expr *init, bool isConst,
                         llvm::ArrayRef<Stmt *> block, size_t index);
  Expr *checkRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkRegexMember(MemberExpr *member);
  Expr *checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkRegExpStatic(CallExpr *call, MemberExpr *member);
  Expr *checkRegExpStaticMember(MemberExpr *member);
  Expr *regexToString(Expr *regex);

private:
  Checker &checker_;
};

}
