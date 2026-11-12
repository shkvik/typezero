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

  bool prepareIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind, llvm::StringRef what);

private:
  using ResultOptionsGiven = Result<const Type *, IntlOptionsCheckerOptionsGivenError>;

  ResultOptionsGiven checkOptionsGiven(const Type *type, IntlKind kind, llvm::StringRef what) const;

  std::optional<std::u16string> literalText(const Expr *expr) const;
  std::optional<double> literalNumber(const Expr *expr) const;
  std::optional<double> plainNumber(const Expr *expr) const;
  const IntlService *serviceOf(IntlKind kind) const;
  bool isSanctionedUnit(llvm::StringRef unit) const;
  bool isWellFormedUnit(llvm::StringRef unit) const;
  bool isKeywordType(llvm::StringRef text) const;
  bool isKeywordPart(llvm::StringRef part) const;
  bool isStringLiteralMember(const Type *member) const;
  std::optional<std::string> knownText(const Expr *value, const Type *type) const;
  std::vector<std::string> literalTexts(const Type *type, bool &open) const;
  const Expr *unconverted(const Expr *expr) const;
  bool isStringLike(const Type *type) const;

  struct IntlOptionsRun {
    const Type *type = nullptr;
    const ObjectLitExpr *literal = nullptr;
    Expr *options = nullptr;
    IntlKind kind = IntlKind::None;
    llvm::StringRef what;
    const IntlService *service = nullptr;
    bool ok = true;
  };

  Expr *valueOf(const IntlOptionsRun &run, llvm::StringRef name) const;
  std::optional<std::string> textOf(const IntlOptionsRun &run, llvm::StringRef name) const;
  std::optional<double> constantOf(const IntlOptionsRun &run, llvm::StringRef name) const;
  const Expr *valueExpr(const IntlOptionsRun &run, llvm::StringRef name) const;
  void reportOption(IntlOptionsRun &run, llvm::StringRef name, const std::string &message);
  const IntlOption *findOption(const IntlService *service, llvm::StringRef name) const;
  void walkOptionField(IntlOptionsRun &run, const Field &field);
  bool isOptionKindFitting(char kind, const Type *value) const;
  bool isBooleanLike(const Type *type) const;
  bool isNumberLike(const Type *type) const;
  bool areAllNumberLike(llvm::ArrayRef<const Type *> members) const;
  bool areAllTextOrBoolean(llvm::ArrayRef<const Type *> members) const;
  void walkOptionValues(IntlOptionsRun &run, const Field &field, const IntlOption &found,
                        const Type *value);
  const std::string *wrongText(const std::vector<std::string> &texts,
                               const IntlOption &found) const;
  void walkOptionTexts(IntlOptionsRun &run);
  void walkStyleOption(IntlOptionsRun &run);
  void walkDigitRanges(IntlOptionsRun &run);
  void walkDigitRange(IntlOptionsRun &run, const char *name, int min, int max);
  bool isNotAbove(const IntlOptionsRun &run, llvm::StringRef low, llvm::StringRef high);
  std::optional<Linear> optionLinear(const IntlOptionsRun &run, llvm::StringRef name,
                                     std::optional<double> constant);
  void walkDigitPairs(IntlOptionsRun &run);
  bool isIncrement(double value) const;
  bool areAllIncrements(const Type *type) const;
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
