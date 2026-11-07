#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/types/unknown_checker/unknown_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class UnknownChecker {
public:
  using ResultValueNarrowed = Result<const Expr *, UnknownCheckerUnknownUseError>;
  using ResultMemberChecked = Result<Expr *, UnknownCheckerCheckUnknownMemberError>;
  using ResultMethodChecked = Result<Expr *, UnknownCheckerCheckUnknownMethodError>;

  explicit UnknownChecker(CheckState &owner);

  static bool holdsUnknown(const Type *type);
  static bool isObjectReference(const Type *type);
  std::string unknownHint(const Expr *expr, const Type *to);
  static const Type *typeOfNarrowed(const std::u16string &name);
  ResultMemberChecked checkUnknownMember(MemberExpr *member);
  ResultMethodChecked checkUnknownMethod(CallExpr *call, MemberExpr *member);
  static ResultValueNarrowed unknownUse(const Expr *object, llvm::StringRef what);
  void addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  static const VarSymbol *checkedVariable(const Expr *expr);
  static std::string hintName(const Expr *expr);

  CheckState &state;
};

}
