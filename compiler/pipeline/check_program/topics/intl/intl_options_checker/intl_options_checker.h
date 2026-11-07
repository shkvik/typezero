#pragma once

#include "compiler/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class IntlOptionsChecker {
public:
  explicit IntlOptionsChecker(CheckState &owner);

  bool checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind, llvm::StringRef what);

private:
  static std::string utf8(const std::u16string &text);
  static std::optional<std::u16string> literalText(const Expr *e);
  static std::optional<double> literalNumber(const Expr *e);
  static const IntlService *serviceOf(IntlKind kind);
  static bool isSanctionedUnit(llvm::StringRef unit);
  static bool isWellFormedUnit(llvm::StringRef unit);
  static bool isKeywordType(llvm::StringRef text);
  static std::string valueList(const std::vector<llvm::StringLiteral> &values);
  static std::optional<std::string> knownText(const Expr *value, const Type *type);
  static std::vector<std::string> literalTexts(const Type *type, bool &open);
  static std::string orList(llvm::ArrayRef<std::string> values);
  static const Expr *unconverted(const Expr *expr);
  static bool isStringLike(const Type *type);

  CheckState &state;
};

}
