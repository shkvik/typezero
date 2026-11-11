#pragma once

#include "compiler/program/library/intl/intl_method_table/intl_method_table.h"
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
  static Expr *toBuiltinCall(CallExpr *call, Builtin builtin, const Type *type);

  Expr *checkLoosely(CallExpr *call);
  void walkRelativeUnit(CallExpr *call);
  Expr *checkSegment(CallExpr *call, MemberExpr *member);
  Expr *checkPluralSelect(CallExpr *call, MemberExpr *member);
  Expr *checkDisplayNameOf(CallExpr *call, MemberExpr *member);
  Expr *checkCollatorCompare(CallExpr *call, MemberExpr *member);
  Expr *checkSegmentsContaining(CallExpr *call, MemberExpr *member);
  Expr *checkListFormat(CallExpr *call, MemberExpr *member, bool toParts);
  Expr *checkNumberFormat(CallExpr *call, MemberExpr *member, bool toParts);
  Expr *checkRelativeFormat(CallExpr *call, MemberExpr *member, bool toParts);
  Expr *checkResolvedOptions(CallExpr *call, MemberExpr *member, IntlKind kind);
  Expr *reportUnsupported(CallExpr *call, MemberExpr *member, const std::string &message);

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
