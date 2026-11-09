#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ByteProofs;
class ExprChecker;
class IndexProofs;
class IntlChecker;
class LiteralTypes;

class IntlOptionsChecker {
public:
  IntlOptionsChecker(SemanticState &owner, SemanticParts &all);

  bool checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind, llvm::StringRef what);

private:
  using ResultOptionsGiven = Result<const Type *, IntlOptionsCheckerOptionsGivenError>;

  static ResultOptionsGiven optionsGiven(const Type *type, IntlKind kind, llvm::StringRef what);

  static std::optional<std::u16string> literalText(const Expr *e);
  static std::optional<double> literalNumber(const Expr *e);
  static std::optional<double> plainNumber(const Expr *e);
  static const IntlService *serviceOf(IntlKind kind);
  static bool isSanctionedUnit(llvm::StringRef unit);
  static bool isWellFormedUnit(llvm::StringRef unit);
  static bool isKeywordType(llvm::StringRef text);
  static std::optional<std::string> knownText(const Expr *value, const Type *type);
  static std::vector<std::string> literalTexts(const Type *type, bool &open);
  static const Expr *unconverted(const Expr *expr);
  static bool isStringLike(const Type *type);

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  IntlChecker *intlChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
};

}
