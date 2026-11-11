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

  struct IntlOptionsRun {
    const Type *type = nullptr;
    const ObjectLitExpr *literal = nullptr;
    Expr *options = nullptr;
    IntlKind kind = IntlKind::None;
    llvm::StringRef what;
    const IntlService *service = nullptr;
    bool ok = true;
  };

  static Expr *valueOf(const IntlOptionsRun &run, llvm::StringRef name);
  static std::optional<std::string> textOf(const IntlOptionsRun &run, llvm::StringRef name);
  static std::optional<double> constantOf(const IntlOptionsRun &run, llvm::StringRef name);
  static const Expr *valueExpr(const IntlOptionsRun &run, llvm::StringRef name);
  void reportOption(IntlOptionsRun &run, llvm::StringRef name, const std::string &message);
  static const IntlOption *findOption(const IntlService *service, llvm::StringRef name);
  void walkOptionField(IntlOptionsRun &run, const Field &field);
  static bool optionKindFits(char kind, const Type *value);
  static bool isBooleanLike(const Type *t);
  static bool isNumberLike(const Type *t);
  static bool allNumberLike(llvm::ArrayRef<const Type *> members);
  static bool allTextOrBoolean(llvm::ArrayRef<const Type *> members);
  void walkOptionValues(IntlOptionsRun &run, const Field &field, const IntlOption &found,
                        const Type *value);
  static const std::string *wrongText(const std::vector<std::string> &texts,
                                      const IntlOption &found);
  void walkOptionTexts(IntlOptionsRun &run);
  void walkStyleOption(IntlOptionsRun &run);
  void walkDigitRanges(IntlOptionsRun &run);
  void walkDigitRange(IntlOptionsRun &run, const char *name, int min, int max);
  bool isNotAbove(const IntlOptionsRun &run, llvm::StringRef low, llvm::StringRef high);
  void walkDigitPairs(IntlOptionsRun &run);
  static bool isIncrement(double value);
  static bool allIncrements(const Type *t);
  void walkRoundingIncrement(IntlOptionsRun &run);
  void walkIncrementDigits(IntlOptionsRun &run);

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  IntlChecker *intlChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
};

}
