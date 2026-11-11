#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker_errors.h"
#include "compiler/semantic/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

IntlChecker::IntlChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->newChecker = &all.getNewChecker();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->literalTypes = &all.getLiteralTypes();
  this->typeRelations = &all.getTypeRelations();
  this->processChecker = &all.getProcessChecker();
  this->argumentChecker = &all.getArgumentChecker();
  this->topLevelChecker = &all.getTopLevelChecker();
  this->intlOptionsChecker = &all.getIntlOptionsChecker();
}

Expr *IntlChecker::kindArg(AstContext &ctx, llvm::SMLoc loc, int kind) {
  auto *lit = ctx.make<IntLitExpr>(loc, uint64_t(kind));
  lit->type = Type::intTy();

  return lit;
}

std::optional<std::u16string> IntlChecker::literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

const IntlService *IntlChecker::serviceNamed(llvm::StringRef name) {
  for (const IntlService &s : IntlCheckHelpers::services()) {
    if (s.name == name) {
      return &s;
    }
  }

  return nullptr;
}

std::u16string IntlChecker::utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

const Type *IntlChecker::relativePartType() {
  return Type::object({{"type", Type::string(), false},
                       {"value", Type::string(), false},
                       {"unit", Type::optional(Type::string()), false}},
                      "Intl.RelativeTimeFormatPart");
}

const Expr *IntlChecker::unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool IntlChecker::isStringLike(const Type *type) {
  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() &&
          llvm::all_of(type->members(), [](const Type *m) { return m->isStringLiteral(); }));
}

const Type *IntlChecker::numberOptionType(llvm::StringRef name) {
  std::vector<int64_t> values;
  if (name == "roundingIncrement") {
    values = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000};
  } else {
    int64_t lo = name.ends_with("FractionDigits") ? 0 : 1;
    int64_t hi = name.ends_with("FractionDigits") ? 100 : 21;
    for (int64_t v = lo; v <= hi; ++v) {
      values.push_back(v);
    }
  }
  std::vector<const Type *> members;
  for (int64_t v : values) {
    members.push_back(Type::numberLiteral(double(v), Type::intTy()));
  }
  const Type *type = Type::unionOf(members);
  if (name != "roundingIncrement") {
    Type::nameUnion(type, std::to_string(values.front()) + ".." + std::to_string(values.back()));
  }

  return type;
}

const Type *IntlChecker::segmentDataType() {
  return Type::object({{"segment", Type::string(), false},
                       {"index", Type::intTy(), false},
                       {"input", Type::string(), false},
                       {"isWordLike", Type::optional(Type::boolTy()), false}},
                      "Intl.SegmentData");
}

const Type *IntlChecker::resolvedIntlOptionsType(IntlKind kind) {
  switch (kind) {
    case IntlKind::NumberFormat:
      return IntlChecker::resolvedNumberOptionsType();
    case IntlKind::Collator:
      return IntlChecker::resolvedCollatorOptionsType();
    case IntlKind::PluralRules:
      return IntlChecker::resolvedPluralOptionsType();
    case IntlKind::RelativeTimeFormat:
      return IntlChecker::resolvedRelativeTimeOptionsType();
    case IntlKind::ListFormat:
      return IntlChecker::resolvedListOptionsType();
    case IntlKind::DisplayNames:
      return IntlChecker::resolvedDisplayNamesOptionsType();
    case IntlKind::None:
    case IntlKind::DateTimeFormat:
    case IntlKind::Segmenter:
    case IntlKind::Segments:
      return IntlChecker::resolvedSegmenterOptionsType();
  }

  return IntlChecker::resolvedSegmenterOptionsType();
}

std::vector<Field> IntlChecker::resolvedDigitFields() {
  const Type *maybeInt = Type::optional(Type::intTy());

  return {{"minimumIntegerDigits", Type::intTy(), false},
          {"minimumFractionDigits", maybeInt, false},
          {"maximumFractionDigits", maybeInt, false},
          {"minimumSignificantDigits", maybeInt, false},
          {"maximumSignificantDigits", maybeInt, false}};
}

std::vector<Field> IntlChecker::resolvedRoundingFields() {
  const Type *text = Type::string();

  return {{"roundingIncrement", Type::intTy(), false},
          {"roundingMode", text, false},
          {"roundingPriority", text, false},
          {"trailingZeroDisplay", text, false}};
}

const Type *IntlChecker::resolvedNumberOptionsType() {
  const Type *text = Type::string();
  const Type *maybe = Type::optional(Type::string());
  std::vector<Field> fields = {
      {"locale", text, false},    {"numberingSystem", text, false},  {"style", text, false},
      {"currency", maybe, false}, {"currencyDisplay", maybe, false}, {"currencySign", maybe, false},
      {"unit", maybe, false},     {"unitDisplay", maybe, false}};
  std::vector<Field> digits = IntlChecker::resolvedDigitFields();
  fields.insert(fields.end(), digits.begin(), digits.end());
  fields.insert(fields.end(),
                {{"useGrouping", Type::unionOf({Type::string(), Type::boolTy()}), false},
                 {"notation", text, false},
                 {"compactDisplay", maybe, false},
                 {"signDisplay", text, false}});
  std::vector<Field> rounding = IntlChecker::resolvedRoundingFields();
  fields.insert(fields.end(), rounding.begin(), rounding.end());

  return Type::object(std::move(fields), "Intl.ResolvedNumberFormatOptions");
}

const Type *IntlChecker::resolvedCollatorOptionsType() {
  const Type *text = Type::string();
  const Type *flag = Type::boolTy();
  std::vector<Field> fields = {{"locale", text, false},      {"usage", text, false},
                               {"sensitivity", text, false}, {"ignorePunctuation", flag, false},
                               {"collation", text, false},   {"numeric", flag, false},
                               {"caseFirst", text, false}};

  return Type::object(std::move(fields), "Intl.ResolvedCollatorOptions");
}

const Type *IntlChecker::resolvedPluralOptionsType() {
  const Type *text = Type::string();
  std::vector<Field> fields = {{"locale", text, false}, {"type", text, false}};
  std::vector<Field> digits = IntlChecker::resolvedDigitFields();
  fields.insert(fields.end(), digits.begin(), digits.end());
  fields.push_back({"pluralCategories", Type::array(Type::string()), false});
  std::vector<Field> rounding = IntlChecker::resolvedRoundingFields();
  fields.insert(fields.end(), rounding.begin(), rounding.end());

  return Type::object(std::move(fields), "Intl.ResolvedPluralRulesOptions");
}

const Type *IntlChecker::resolvedRelativeTimeOptionsType() {
  const Type *text = Type::string();
  std::vector<Field> fields = {{"locale", text, false},
                               {"style", text, false},
                               {"numeric", text, false},
                               {"numberingSystem", text, false}};

  return Type::object(std::move(fields), "Intl.ResolvedRelativeTimeFormatOptions");
}

const Type *IntlChecker::resolvedListOptionsType() {
  const Type *text = Type::string();
  std::vector<Field> fields = {
      {"locale", text, false}, {"type", text, false}, {"style", text, false}};

  return Type::object(std::move(fields), "Intl.ResolvedListFormatOptions");
}

const Type *IntlChecker::resolvedDisplayNamesOptionsType() {
  const Type *text = Type::string();
  const Type *maybe = Type::optional(Type::string());
  std::vector<Field> fields = {{"locale", text, false},
                               {"style", text, false},
                               {"type", text, false},
                               {"fallback", text, false},
                               {"languageDisplay", maybe, false}};

  return Type::object(std::move(fields), "Intl.ResolvedDisplayNamesOptions");
}

const Type *IntlChecker::resolvedSegmenterOptionsType() {
  const Type *text = Type::string();
  std::vector<Field> fields = {{"locale", text, false}, {"granularity", text, false}};

  return Type::object(std::move(fields), "Intl.ResolvedSegmenterOptions");
}

const Type *IntlChecker::intlTypeNamed(llvm::StringRef name) {
  if (name == "DateTimeFormat") {
    return Type::dateFormat();
  }
  if (const IntlService *service = IntlChecker::serviceNamed(name)) {
    return Type::intl(service->kind);
  }
  if (name == "Segments") {
    return Type::intl(IntlKind::Segments);
  }
  if (name == "SegmentData") {
    return IntlChecker::segmentDataType();
  }
  if (name == "DateTimeFormatPart" || name == "NumberFormatPart") {
    return DateFormatChecker::dateFormatPartType();
  }
  if (name == "RelativeTimeFormatPart") {
    return IntlChecker::relativePartType();
  }
  if (name == "ResolvedDateTimeFormatOptions") {
    return DateFormatChecker::resolvedDateOptionsType();
  }
  if (name == "DateTimeFormatOptions") {
    return DateFormatChecker::dateOptionsType();
  }
  if (name == "LDMLPluralRule" || name == "RelativeTimeFormatUnit" ||
      name == "UnicodeBCP47LocaleIdentifier" || name == "BCP47LanguageTag") {
    return Type::string();
  }
  for (const IntlService &service : IntlCheckHelpers::services()) {
    if (name == "Resolved" + service.name.str() + "Options") {
      return IntlChecker::resolvedIntlOptionsType(service.kind);
    }
    if (name != service.name.str() + "Options") {
      continue;
    }
    std::vector<Field> fields;
    for (const IntlOption &option : service.options) {
      const Type *type = option.kind == 's'   ? Type::string()
                         : option.kind == 'n' ? Type::doubleTy()
                         : option.kind == 'b' ? Type::boolTy()
                                              : Type::unionOf({Type::string(), Type::boolTy()});
      if (option.kind == 'n') {
        type = IntlChecker::numberOptionType(option.name);
      }
      if ((option.kind == 's' || option.kind == 'g') && !option.values.empty()) {
        std::vector<const Type *> members;
        for (llvm::StringLiteral value : option.values) {
          members.push_back(Type::stringLiteral(IntlChecker::utf16(value)));
        }
        if (option.kind == 'g') {
          members.push_back(Type::boolTy());
        }
        type = Type::unionOf(members);
      }
      fields.push_back({option.name.str(), Type::optional(type), false});
    }
    return Type::object(std::move(fields), "Intl." + service.name.str() + "Options");
  }

  return nullptr;
}

Expr *IntlChecker::checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                                bool withNew) {
  auto failed = [&] {
    for (Expr *&arg : args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  const IntlService *service = IntlChecker::serviceNamed(name);
  if (!service) {
    std::string message = ErrorTextIntlNew::unknown(name);
    this->state.getDiagnostics().error(loc, message);
    return failed();
  }
  if (!withNew && !service->callable) {
    std::string message = ErrorTextIntlNew::withNew(name);
    this->state.getDiagnostics().error(loc, message);
    return failed();
  }
  if (args.size() > 2) {
    std::string message = ErrorTextDateFormat::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    return failed();
  }
  if (service->kind == IntlKind::DisplayNames && args.size() < 2) {
    std::string message = ErrorTextIntlNew::noType();
    this->state.getDiagnostics().error(loc, message);
    return failed();
  }
  std::string what = ErrorTextIntlNew::label(name);
  if (!this->intlOptionsChecker->checkIntlArgs(args, 0, service->kind, what)) {
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  std::vector<Expr *> all{IntlChecker::kindArg(this->state.getAst(), loc, service->runtimeKind)};
  all.insert(all.end(), args.begin(), args.end());

  return this->processChecker->builtinCall(loc, this->state.getAst().make<NameExpr>(loc, "Intl"),
                                           Builtin::IntlNew, std::move(all),
                                           Type::intl(service->kind));
}

IntlChecker::ResultLanguageTags IntlChecker::languageTags(const Expr *arg, llvm::StringRef what,
                                                          bool constructor) {
  std::optional<std::vector<std::string>> tags = this->literalTypes->knownTexts(arg);
  if (!tags) {
    std::string message = ErrorTextLanguageTag::unknown(constructor, what);

    return err(IntlCheckerLanguageTagsError{
        .code = IntlCheckerLanguageTagsErrorCode::UNKNOWN_TAGS,
        .message = message,
    });
  }
  for (const std::string &tag : *tags) {
    if (!DateFormatChecker::isLanguageTag(tag)) {
      std::string message = ErrorTextLanguageTag::notTag(tag);

      return err(IntlCheckerLanguageTagsError{
          .code = IntlCheckerLanguageTagsErrorCode::NOT_A_TAG,
          .message = message,
      });
    }
  }

  return ok(*tags);
}

bool IntlChecker::checkLocaleArg(Expr *&arg, llvm::StringRef what) {
  const Type *type = arg->type;
  if (type->isError()) {
    return false;
  }
  if (type->isUndefined()) {
    return true;
  }
  if (ClassDecl *locale = this->state.getLibraryDeclarations().findLibraryClass("Locale");
      locale && !type->isOptional() && ClassDeclarer::classOf(type) == locale) {
    auto *tag = this->state.getAst().make<MemberExpr>(arg->loc(), arg, "tag");
    tag->type = Type::string();
    arg = tag;
    return true;
  }
  if (!IntlChecker::isStringLike(type->nonOptional())) {
    std::string message = ErrorTextLanguageTag::takes(what, type->name(), type->isArray());
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  ResultLanguageTags tags = this->languageTags(arg, what, false);
  if (tags.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), tags.error().message);
    return false;
  }
  if (!type->nonOptional()->isString()) {
    arg = this->typeRelations->convert(arg, type->isOptional() ? Type::optional(Type::string())
                                                               : Type::string());
  }

  return true;
}

Expr *IntlChecker::checkLocaleList(Expr *arg, llvm::StringRef what) {
  bool array = isa<ArrayLitExpr>(arg);
  arg = this->exprChecker->checkExpr(arg, array ? Type::array(Type::string()) : nullptr);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isArray()) {
    this->checkLocaleArg(arg, what);
    return arg;
  }
  if (type != Type::array(Type::string())) {
    std::string message = ErrorTextLanguageTag::list(what, type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  auto *list = dyn_cast<ArrayLitExpr>(IntlChecker::unconverted(arg));
  if (!list) {
    std::string message = ErrorTextLanguageTag::listOpen(what);
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  for (const ArrayLitExpr::Element &e : list->elements) {
    if (e.spread) {
      std::string message = ErrorTextLanguageTag::spread(what);
      this->state.getDiagnostics().error(e.expr->loc(), message);
      return arg;
    }
    ResultLanguageTags tags = this->languageTags(e.expr, what, false);
    if (tags.isErr()) {
      this->state.getDiagnostics().error(e.expr->loc(), tags.error().message);
      return arg;
    }
  }

  return arg;
}

Expr *IntlChecker::checkLocaleNew(NewExpr *expr, const Type *expected) {
  if (!this->topLevelChecker->lookupValue(LibraryModuleNames::kIntlLocaleName).cls()) {
    std::string message = ErrorTextIntlLocale::missing();
    this->state.getDiagnostics().error(expr->loc(), message);
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return expr;
  }
  if (expr->args.size() == 2) {
    std::string message = ErrorTextIntlLocale::options();
    this->state.getDiagnostics().error(expr->args[1]->loc(), message);
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return expr;
  }
  expr->name = LibraryModuleNames::kIntlLocaleName;
  expr->member = "";
  Expr *checked = this->newChecker->checkNew(expr, expected);
  auto *made = dyn_cast<NewExpr>(checked);
  std::vector<Expr *> *args = made ? (made->ctor() ? &made->ctor()->args : &made->args) : nullptr;
  if (args && args->size() == 1 && args->front()->type && !args->front()->type->isError()) {
    std::string what = ErrorTextIntlLocale::label();
    ResultLanguageTags tags = this->languageTags(args->front(), what, true);
    if (tags.isErr()) {
      this->state.getDiagnostics().error(args->front()->loc(), tags.error().message);
    }
  }

  return checked;
}

Expr *IntlChecker::checkLocaleStatic(CallExpr *call, MemberExpr *member, const Type *expected) {
  if (!this->topLevelChecker->lookupValue(LibraryModuleNames::kIntlLocaleName).cls()) {
    std::string message = ErrorTextIntlLocale::missing();
    this->state.getDiagnostics().error(member->loc(), message);
    call->type = Type::error();
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  member->object = this->state.getAst().make<NameExpr>(member->object->loc(),
                                                       LibraryModuleNames::kIntlLocaleName);

  return this->callChecker->checkCall(call, expected);
}

Expr *IntlChecker::checkIntlFunction(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  if (member->name == "getCanonicalLocales") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      std::string what = ErrorTextIntlFunction::canonical();
      call->args[0] = this->checkLocaleList(call->args[0], what);
    }
    call->target = BuiltinCall{.builtin = Builtin::IntlCanonicalLocales};
    call->type = Type::array(Type::string());
    return call;
  }
  if (member->name == "supportedValuesOf") {
    std::string message = ErrorTextIntlFunction::values();
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  if (member->name == "Locale") {
    std::string message = ErrorTextIntlLocale::withNew();
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }

  return this->checkIntlNew(member->loc(), member->name, call->args, false);
}

Expr *IntlChecker::checkIntlStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto *service = cast<MemberExpr>(member->object);
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  std::string what = ErrorTextIntlFunction::staticOf(service->name, member->name);
  if ((service->name != "DateTimeFormat" && !IntlChecker::serviceNamed(service->name)) ||
      member->name != "supportedLocalesOf") {
    std::string message = ErrorTextIntlFunction::onlyLocale(what);
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->checkLocaleList(call->args[0], what);
  if (call->args[0]->type->isError()) {
    return call;
  }
  if (call->args.size() == 2) {
    std::vector<Expr *> pair{this->state.getAst().make<UndefinedExpr>(member->loc()),
                             call->args[1]};
    if (!this->intlOptionsChecker->checkIntlArgs(pair, 0, IntlKind::None, what)) {
      return call;
    }
    call->args[1] = pair[1];
  }
  call->args.insert(call->args.begin(), this->state.getAst().make<StringLitExpr>(
                                            member->loc(), IntlChecker::utf16(what)));
  call->args[0]->type = Type::string();
  call->target = BuiltinCall{.builtin = Builtin::IntlSupportedLocales};
  call->type = Type::array(Type::string());

  return call;
}

Expr *IntlChecker::checkIntlMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  IntlKind kind = member->object->type->intlKind();
  llvm::StringRef name = member->name;
  const Type *wantedParam = nullptr;
  const Type *wantedRet = nullptr;
  if (this->state.getFunctionStack().getExpectedCallback() &&
      !this->state.getFunctionStack().getExpectedCallback()->params.empty()) {
    wantedParam = this->state.getFunctionStack().getExpectedCallback()->params[0];
    wantedRet = this->state.getFunctionStack().getExpectedCallback()->ret;
  } else if (expected && expected->nonOptional()->isFunction()) {
    const Type *fn = expected->nonOptional();
    wantedParam = fn->params().empty() ? nullptr : fn->params()[0];
    wantedRet = fn->ret();
  }
  const Type *fnType = nullptr;
  if (kind == IntlKind::NumberFormat && name == "format") {
    const Type *param = Type::doubleTy();
    if (wantedParam && (wantedParam->isNumeric() || wantedParam->isDecimal())) {
      param = wantedParam;
    }
    fnType = Type::function({param}, 1, false, Type::string());
  } else if (kind == IntlKind::DateTimeFormat && name == "format") {
    const Type *param = Type::date();
    if (wantedParam && wantedParam->isNumeric()) {
      param = wantedParam;
    }
    fnType = Type::function({param}, 1, false, Type::string());
  } else if (kind == IntlKind::Collator && name == "compare") {
    const Type *ret = wantedRet && wantedRet->isFloat() ? Type::doubleTy() : Type::intTy();
    fnType = Type::function({Type::string(), Type::string()}, 2, false, ret);
  }
  if (!fnType) {
    std::string message = kind == IntlKind::DateTimeFormat
                              ? ErrorTextIntlMember::dateFormat(name)
                              : ErrorTextIntlMember::onlyCalled(name, member->object->type->name());
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  member->type = fnType;

  return this->processChecker->builtinCall(member->loc(), member, Builtin::IntlBound, {}, fnType);
}

Expr *IntlChecker::segmentsArray(Expr *segments) {
  auto *member = this->state.getAst().make<MemberExpr>(segments->loc(), segments, "%segments");
  const Type *type = Type::array(IntlChecker::segmentDataType());
  member->type = type;

  return this->processChecker->builtinCall(segments->loc(), member, Builtin::SegmentsArray, {},
                                           type);
}

}
