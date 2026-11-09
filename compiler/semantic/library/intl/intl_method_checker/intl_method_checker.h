#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/intl/intl_method_checker/intl_method_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class MathChecker;
class LiteralTypes;
class TypeRelations;
class GenericChecker;
class ArgumentChecker;
class IntlOptionsChecker;

class IntlMethodChecker {
public:
  IntlMethodChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkIntlMethod(CallExpr *call, MemberExpr *member);
  Expr *checkNumberLocale(CallExpr *call, MemberExpr *member);
  Expr *checkStringLocale(CallExpr *call, MemberExpr *member);
  Expr *checkIntlNumber(Expr *arg, llvm::StringRef method);

private:
  static std::optional<std::u16string> literalText(const Expr *e);
  static const Type *relativePartType();
  using ResultClosedText = Result<const Expr *, IntlMethodCheckerClosedArgError>;
  using ResultIntlNumber = Result<const Type *, IntlMethodCheckerIntlNumberError>;

  static ResultIntlNumber intlNumber(const Type *type, llvm::StringRef method);
  static ResultClosedText closedArg(const Expr *arg,
                                    const std::optional<std::vector<std::string>> &texts,
                                    llvm::ArrayRef<llvm::StringLiteral> allowed,
                                    llvm::StringRef what);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  MathChecker *mathChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IntlOptionsChecker *intlOptionsChecker = nullptr;
};

}
