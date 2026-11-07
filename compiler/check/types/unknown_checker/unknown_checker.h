#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class UnknownChecker {
public:
  using ResultValueNarrowed = Result<const Expr *, UnknownCheckerUnknownUseError>;
  using ResultMemberChecked = Result<Expr *, UnknownCheckerCheckUnknownMemberError>;
  using ResultMethodChecked = Result<Expr *, UnknownCheckerCheckUnknownMethodError>;

  explicit UnknownChecker(Checker &owner);

  static bool holdsUnknown(const Type *type);
  static bool isObjectReference(const Type *type);
  std::string unknownHint(const Expr *expr, const Type *to);
  static const Type *typeOfNarrowed(const std::u16string &name);
  ResultMemberChecked checkUnknownMember(MemberExpr *member);
  ResultMethodChecked checkUnknownMethod(CallExpr *call, MemberExpr *member);
  static ResultValueNarrowed unknownUse(const Expr *object, llvm::StringRef what);
  void addUnknownFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;

private:
  static const VarSymbol *checkedVariable(const Expr *expr);
  static std::string hintName(const Expr *expr);

  Checker &checker;
};

}
