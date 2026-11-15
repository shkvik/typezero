#pragma once

#include "compiler/program/library/date/date_format_option_table/date_format_option_table.h"
#include "compiler/program/library/intl/intl_method_table/intl_method_table.h"
#include "compiler/program/library/intl/intl_service_table/intl_service_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/intl/library_intl_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tz {

class LibraryIntlRegistry {
public:
  explicit LibraryIntlRegistry(SemanticState &owner);

  void afterLocaleNew();
  void findIntlType(const FindIntlType &task);
  void checkIntlNew(const CheckIntlNew &task);
  void localeArgSlot(const LocaleArgSlot &task);
  void checkIntlCall(const CheckIntlCall &task);
  void finishIntlNew(const FinishIntlNew &task);
  void localeListSlot(const LocaleListSlot &task);
  void intlOptionsArg(const IntlOptionsArg &task);
  void checkLocaleNew(const CheckLocaleNew &task);
  void afterIntlNumber(const AfterIntlNumber &task);
  void afterLocaleList(const AfterLocaleList &task);
  void checkIntlStatic(const CheckIntlStatic &task);
  void checkIntlMember(const CheckIntlMember &task);
  void checkIntlMethod(const CheckIntlMethod &task);
  void prepareIntlArgs(const PrepareIntlArgs &task);
  void afterIntlOptions(const AfterIntlOptions &task);
  void afterDateOptions(const AfterDateOptions &task);
  void finishLocaleCall(const FinishLocaleCall &task);
  void checkLocaleStatic(const CheckLocaleStatic &task);
  void afterDoubleNumber(const AfterDoubleNumber &task);
  void afterRelativeUnit(const AfterRelativeUnit &task);
  void checkNumberLocale(const CheckNumberLocale &task);
  void checkStringLocale(const CheckStringLocale &task);
  void segmentsArraySlot(const SegmentsArraySlot &task);
  void prepareLocaleArgs(const PrepareLocaleArgs &task);
  void dateLocaleOptions(const DateLocaleOptions &task);
  void checkDateFormatNew(const CheckDateFormatNew &task);
  void afterNormalizeForm(const AfterNormalizeForm &task);
  void afterDateFormatArg(const AfterDateFormatArg &task);
  void finishDateFormatNew(const FinishDateFormatNew &task);
  void afterStaticLocaleList(const AfterStaticLocaleList &task);
  void checkDateFormatMethod(const CheckDateFormatMethod &task);
  void finishSupportedLocales(const FinishSupportedLocales &task);

private:
  enum class IntlCreation {
    NEW,
    CALL,
  };

  enum class LanguageTagUse {
    LOCALE_CONSTRUCTOR,
    LOCALE_ARGUMENT,
  };

  enum class DateOptionRole {
    Setting,
    Component,
    Style,
  };

  struct WantedCallback {
    const Type *param;
    const Type *ret;
  };

  struct IntlOptionsRun {
    const Type *type = nullptr;
    const ObjectLitExpr *literal = nullptr;
    Expr *options = nullptr;
    IntlKind kind = IntlKind::None;
    llvm::StringRef what;
    const IntlService *service = nullptr;
    bool ok = true;
  };

  struct DateOptionPlace {
    const Expr *given;
    llvm::SMLoc at;
    bool isExempt;
  };

  struct OptionRoles {
    bool hasStyle;
    bool hasComponent;
  };

  using ResultIntlNumber = Result<const Type *, LibraryIntlRegistryIntlNumberError>;
  using ResultClosedText = Result<const Expr *, LibraryIntlRegistryClosedArgError>;
  using ResultOptionsGiven = Result<const Type *, LibraryIntlRegistryOptionsGivenError>;
  using ResultOptionsObject = Result<const Type *, LibraryIntlRegistryOptionsObjectError>;
  using ResultLocalePresent = Result<size_t, LibraryIntlRegistryLocalePresentError>;
  using ResultIntlCreation = Result<const IntlService *, LibraryIntlRegistryIntlCreationError>;
  using ResultLanguageTags = Result<std::vector<std::string>, LibraryIntlRegistryLanguageTagsError>;

  void pushDoubleNumber(Expr **slot);
  void checkNormalize(CallExpr *call);
  void walkOptionTexts(IntlOptionsRun &run);
  void walkStyleOption(IntlOptionsRun &run);
  void walkDigitRanges(IntlOptionsRun &run);
  void walkDigitPairs(IntlOptionsRun &run);
  void pushRelativeFormat(CallExpr *call);
  void walkRoundingIncrement(IntlOptionsRun &run);
  void walkIncrementDigits(IntlOptionsRun &run);
  void walkArgsLoosely(std::vector<Expr *> *args);
  void finishDateFormatCall(CallExpr *call, bool isFormat);
  void pushIntlNumber(Expr **slot, llvm::StringRef method);
  void checkIntlFunction(CallExpr *call, MemberExpr *member);
  void walkOptionField(IntlOptionsRun &run, const Field &field);
  void failIntlNew(llvm::SMLoc loc, std::vector<Expr *> *args);
  void startDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> *args);
  void checkCanonicalLocales(CallExpr *call, const MemberExpr *member);
  void walkLocaleElements(const ArrayLitExpr *list, llvm::StringRef what);
  void walkDigitRange(IntlOptionsRun &run, const IntlDigitRange &range);
  void toBuiltinCall(CallExpr *call, Builtin builtin, const Type *type) const;
  void reportOption(IntlOptionsRun &run, IntlOptionKey key, const std::string &message);
  void reportOptionNamed(IntlOptionsRun &run, llvm::StringRef name, const std::string &message);
  void reportUnsupported(CallExpr *call, const MemberExpr *member, const std::string &message);
  void checkIntlMethodForm(CallExpr *call, MemberExpr *member, IntlMethod method, IntlKind kind);
  void startIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> *args,
                    IntlCreation creation);
  void walkOptionValues(IntlOptionsRun &run, const Field &field, const IntlOption &found,
                        const Type *value);


  bool hasLocaleClass() const;
  bool isIncrement(double value) const;
  bool isOutsideDigits(double digits) const;
  bool isNumberLike(const Type *type) const;
  bool isStringLike(const Type *type) const;
  bool isBooleanLike(const Type *type) const;
  bool areAllIncrements(const Type *type) const;
  bool matchOptionValues(const Expr *options);
  bool isKeywordPart(llvm::StringRef part) const;
  bool isKeywordType(llvm::StringRef text) const;
  bool isLanguageTag(const std::string &tag) const;
  bool isStringLiteralMember(const Type *member) const;
  bool matchOptionKinds(const Expr *options, int kind);
  bool prepareLocaleArg(Expr **slot, llvm::StringRef what);
  bool areAllNumberLike(llvm::ArrayRef<const Type *> members) const;
  bool isOptionKindFitting(IntlOptionKind kind, const Type *value) const;
  bool areAllTextOrBoolean(llvm::ArrayRef<const Type *> members) const;
  bool matchDigits(const Field &field, const DateOptionPlace &place);
  bool matchOptionText(const Field &field, const DateOptionPlace &place);
  bool matchStyleMethod(const Expr *options, llvm::StringRef name, int kind);
  bool isNotAbove(const IntlOptionsRun &run, IntlOptionKey low, IntlOptionKey high);
  bool matchOptionKind(const Expr *options, const Field &field,
                       std::optional<DateFormatOption> option, int kind);
  bool matchOptionRole(const Expr *options, const Field &field, DateFormatOption option, int kind,
                       OptionRoles &roles);


  Expr *failedValue(llvm::SMLoc loc);
  Expr *kindArg(llvm::SMLoc loc, int kind);
  Expr *valueOf(const IntlOptionsRun &run, llvm::StringRef name) const;


  const Expr *unconverted(const Expr *expr) const;
  const Expr *valueExpr(const IntlOptionsRun &run, IntlOptionKey key) const;
  const Expr *givenProperty(const ObjectLitExpr *literal, llvm::StringRef name) const;


  std::optional<double> plainNumber(const Expr *expr) const;
  std::optional<double> literalNumber(const Expr *expr) const;
  std::optional<double> constantOf(const IntlOptionsRun &run, IntlOptionKey key) const;


  std::optional<std::string> knownText(const Expr *value, const Type *type) const;
  std::optional<std::string> textOf(const IntlOptionsRun &run, IntlOptionKey key) const;


  std::optional<std::vector<std::string>> textsOf(const Type *type) const;
  std::optional<std::vector<std::string>> optionTexts(const Field &field,
                                                      const DateOptionPlace &place);


  ResultLocalePresent checkLocalePresent(size_t count) const;
  ResultOptionsObject checkOptionsObject(const Type *type, int kind) const;
  ResultIntlNumber checkIntlNumberType(const Type *type, llvm::StringRef method) const;
  ResultOptionsGiven checkOptionsGiven(const Type *type, IntlKind kind, llvm::StringRef what) const;
  ResultLanguageTags checkLanguageTags(const Expr *arg, llvm::StringRef what, LanguageTagUse use);
  ResultIntlCreation checkIntlCreation(const IntlService *service, llvm::StringRef name,
                                       size_t count, IntlCreation creation) const;
  ResultClosedText checkClosedArg(const Expr *arg,
                                  const std::optional<std::vector<std::string>> &texts,
                                  llvm::ArrayRef<llvm::StringLiteral> allowed,
                                  llvm::StringRef what) const;


  WantedCallback wantedCallback(const Type *expected) const;
  DateOptionRole optionRole(DateFormatOption option) const;
  std::optional<std::u16string> literalText(const Expr *expr) const;
  std::vector<Expr *> *constructedArgs(NewExpr *made) const;
  std::pair<unsigned, unsigned> intlMethodArity(IntlMethod method) const;
  std::optional<std::vector<double>> numbersOf(const Type *type) const;
  const Field *fieldOf(const IntlOptionsRun &run, IntlOptionKey key) const;
  std::vector<std::string> literalTexts(const Type *type, bool &open) const;
  const Type *wantedOptionType(DateFormatOption option, const Type *value) const;
  const IntlOption *findOption(const IntlService *service, llvm::StringRef name) const;
  const std::string *wrongText(const std::vector<std::string> &texts,
                               const IntlOption &found) const;
  std::optional<Linear> optionLinear(const IntlOptionsRun &run, IntlOptionKey key,
                                     std::optional<double> constant);
  const Type *boundFunctionType(IntlKind kind, llvm::StringRef name,
                                const WantedCallback &wanted) const;

  SemanticState &state;
};

}
