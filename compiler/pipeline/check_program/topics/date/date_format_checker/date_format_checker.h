#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/date/date_format_checker/date_format_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class DateFormatChecker {
public:
  static bool isLanguageTag(const std::string &tag);
  explicit DateFormatChecker(CheckState &owner);

  bool checkLocaleArgs(std::vector<Expr *> &args, int kind);
  Expr *checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkIntlCall(CallExpr *call, MemberExpr *member);
  Expr *checkDateFormatMethod(CallExpr *call, MemberExpr *member);
  static const Type *dateFormatPartType();
  static const Type *resolvedDateOptionsType();
  static const Type *dateOptionsType();

private:
  using ResultOptionsObject = Result<const Type *, DateFormatCheckerOptionsObjectError>;

  static ResultOptionsObject optionsObject(const Type *type, int kind);
  static std::optional<std::u16string> literalText(const Expr *e);
  static std::optional<std::vector<std::string>> textsOf(const Type *type);
  static std::optional<std::vector<double>> numbersOf(const Type *type);
  static const char *methodName(int kind);

  CheckState &state;
};

}
