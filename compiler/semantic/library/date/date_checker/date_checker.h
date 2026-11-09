#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/date/date_checker/date_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class CallChecker;
class ExprChecker;
class TypeRelations;
class ConstantFolder;
class GenericChecker;
class ProcessChecker;
class ArgumentChecker;
class DateFormatChecker;

class DateChecker {
public:
  DateChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkDateNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *timeArg(Expr *arg);
  Expr *checkDateFunction(CallExpr *call, NameExpr *name);
  Expr *checkDateStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDateStaticMember(MemberExpr *member);
  Expr *checkDateMethod(CallExpr *call, MemberExpr *member);
  Expr *checkDateMember(MemberExpr *member);
  Expr *checkDateOperands(BinaryExpr *bin);
  Expr *dateTime(Expr *date);
  Expr *dateToString(Expr *date);
  void checkDateLiteral(const Expr *arg);
  void checkDateFields(std::vector<Expr *> &args, size_t first = 0, llvm::StringRef setter = "");

private:
  using ResultLiteralDate = Result<double, DateCheckerLiteralDateError>;

  static ResultLiteralDate literalDate(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static bool isLeap(int64_t year);
  static int daysInMonth(int64_t year, int64_t month);
  static std::string missingDay(std::optional<int64_t> year, int64_t month, int64_t day);
  static std::pair<int64_t, int64_t> carriedDay(int64_t year, int64_t month, int64_t day);

  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  DateFormatChecker *dateFormatChecker = nullptr;
};

}
