#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
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
  this->newChecker = &all.newChecker;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->literalTypes = &all.literalTypes;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
  this->topLevelChecker = &all.topLevelChecker;
  this->dateFormatChecker = &all.dateFormatChecker;
  this->intlOptionsChecker = &all.intlOptionsChecker;
}

Expr *IntlChecker::kindArg(AstContext &context, llvm::SMLoc loc, int kind) const {
  auto *literal = context.make<IntLitExpr>(loc, uint64_t(kind));
  literal->type = Type::intTy();

  return literal;
}

std::optional<std::u16string> IntlChecker::literalText(const Expr *expr) const {
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

const IntlService *IntlChecker::serviceNamed(llvm::StringRef name) const {
  for (const IntlService &s : IntlCheckHelpers::services()) {
    if (s.name == name) {
      return &s;
    }
  }

  return nullptr;
}

const Type *IntlChecker::relativePartType() const {
  return Type::object({{"type", Type::string(), false},
                       {"value", Type::string(), false},
                       {"unit", Type::optional(Type::string()), false}},
                      "Intl.RelativeTimeFormatPart");
}

const Expr *IntlChecker::unconverted(const Expr *expr) const {
  expr = ExprParts::withoutImplicitCasts(expr);

  return expr;
}

bool IntlChecker::isStringLike(const Type *type) const {
  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() && llvm::all_of(type->members(), [](const Type *member) {
            return member->isStringLiteral();
          }));
}

const Type *IntlChecker::numberOptionType(llvm::StringRef name) const {
  auto values = std::vector<int64_t>();
  if (name == "roundingIncrement") {
    values = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000};
  } else {
    int64_t low = name.ends_with("FractionDigits") ? 0 : 1;
    int64_t high = name.ends_with("FractionDigits") ? 100 : 21;
    for (int64_t v = low; v <= high; ++v) {
      values.push_back(v);
    }
  }
  auto members = std::vector<const Type *>();
  for (int64_t v : values) {
    members.push_back(Type::numberLiteral(double(v), Type::intTy()));
  }
  const auto *type = Type::unionOf(members);
  if (name != "roundingIncrement") {
    Type::nameUnion(type, std::to_string(values.front()) + ".." + std::to_string(values.back()));
  }

  return type;
}

const Type *IntlChecker::segmentDataType() const {
  return Type::object({{"segment", Type::string(), false},
                       {"index", Type::intTy(), false},
                       {"input", Type::string(), false},
                       {"isWordLike", Type::optional(Type::boolTy()), false}},
                      "Intl.SegmentData");
}

const Type *IntlChecker::resolvedIntlOptionsType(IntlKind kind) const {
  switch (kind) {
    case IntlKind::NumberFormat:
      return this->resolvedNumberOptionsType();
    case IntlKind::Collator:
      return this->resolvedCollatorOptionsType();
    case IntlKind::PluralRules:
      return this->resolvedPluralOptionsType();
    case IntlKind::RelativeTimeFormat:
      return this->resolvedRelativeTimeOptionsType();
    case IntlKind::ListFormat:
      return this->resolvedListOptionsType();
    case IntlKind::DisplayNames:
      return this->resolvedDisplayNamesOptionsType();
    case IntlKind::None:
    case IntlKind::DateTimeFormat:
    case IntlKind::Segmenter:
    case IntlKind::Segments:
      return this->resolvedSegmenterOptionsType();
  }

  return this->resolvedSegmenterOptionsType();
}

std::vector<Field> IntlChecker::resolvedDigitFields() const {
  const auto *maybeInt = Type::optional(Type::intTy());

  return {{"minimumIntegerDigits", Type::intTy(), false},
          {"minimumFractionDigits", maybeInt, false},
          {"maximumFractionDigits", maybeInt, false},
          {"minimumSignificantDigits", maybeInt, false},
          {"maximumSignificantDigits", maybeInt, false}};
}

std::vector<Field> IntlChecker::resolvedRoundingFields() const {
  const auto *text = Type::string();

  return {{"roundingIncrement", Type::intTy(), false},
          {"roundingMode", text, false},
          {"roundingPriority", text, false},
          {"trailingZeroDisplay", text, false}};
}

const Type *IntlChecker::resolvedNumberOptionsType() const {
  const auto *text = Type::string();
  const auto *maybe = Type::optional(Type::string());
  std::vector<Field> fields = {
      {"locale", text, false},    {"numberingSystem", text, false},  {"style", text, false},
      {"currency", maybe, false}, {"currencyDisplay", maybe, false}, {"currencySign", maybe, false},
      {"unit", maybe, false},     {"unitDisplay", maybe, false}};
  auto digits = this->resolvedDigitFields();
  fields.insert(fields.end(), digits.begin(), digits.end());
  fields.insert(fields.end(),
                {{"useGrouping", Type::unionOf({Type::string(), Type::boolTy()}), false},
                 {"notation", text, false},
                 {"compactDisplay", maybe, false},
                 {"signDisplay", text, false}});
  auto rounding = this->resolvedRoundingFields();
  fields.insert(fields.end(), rounding.begin(), rounding.end());

  return Type::object(std::move(fields), "Intl.ResolvedNumberFormatOptions");
}

const Type *IntlChecker::resolvedCollatorOptionsType() const {
  const auto *text = Type::string();
  const auto *flag = Type::boolTy();
  std::vector<Field> fields = {{"locale", text, false},      {"usage", text, false},
                               {"sensitivity", text, false}, {"ignorePunctuation", flag, false},
                               {"collation", text, false},   {"numeric", flag, false},
                               {"caseFirst", text, false}};

  return Type::object(std::move(fields), "Intl.ResolvedCollatorOptions");
}

const Type *IntlChecker::resolvedPluralOptionsType() const {
  const auto *text = Type::string();
  std::vector<Field> fields = {{"locale", text, false}, {"type", text, false}};
  auto digits = this->resolvedDigitFields();
  fields.insert(fields.end(), digits.begin(), digits.end());
  fields.push_back({"pluralCategories", Type::array(Type::string()), false});
  auto rounding = this->resolvedRoundingFields();
  fields.insert(fields.end(), rounding.begin(), rounding.end());

  return Type::object(std::move(fields), "Intl.ResolvedPluralRulesOptions");
}

const Type *IntlChecker::resolvedRelativeTimeOptionsType() const {
  const auto *text = Type::string();
  std::vector<Field> fields = {{"locale", text, false},
                               {"style", text, false},
                               {"numeric", text, false},
                               {"numberingSystem", text, false}};

  return Type::object(std::move(fields), "Intl.ResolvedRelativeTimeFormatOptions");
}

const Type *IntlChecker::resolvedListOptionsType() const {
  const auto *text = Type::string();
  std::vector<Field> fields = {
      {"locale", text, false}, {"type", text, false}, {"style", text, false}};

  return Type::object(std::move(fields), "Intl.ResolvedListFormatOptions");
}

const Type *IntlChecker::resolvedDisplayNamesOptionsType() const {
  const auto *text = Type::string();
  const auto *maybe = Type::optional(Type::string());
  std::vector<Field> fields = {{"locale", text, false},
                               {"style", text, false},
                               {"type", text, false},
                               {"fallback", text, false},
                               {"languageDisplay", maybe, false}};

  return Type::object(std::move(fields), "Intl.ResolvedDisplayNamesOptions");
}

const Type *IntlChecker::resolvedSegmenterOptionsType() const {
  const auto *text = Type::string();
  std::vector<Field> fields = {{"locale", text, false}, {"granularity", text, false}};

  return Type::object(std::move(fields), "Intl.ResolvedSegmenterOptions");
}

const Type *IntlChecker::intlTypeNamed(llvm::StringRef name) const {
  if (name == "DateTimeFormat") {
    return Type::dateFormat();
  }
  if (const IntlService *service = this->serviceNamed(name)) {
    return Type::intl(service->kind);
  }
  if (name == "Segments") {
    return Type::intl(IntlKind::Segments);
  }
  if (name == "SegmentData") {
    return this->segmentDataType();
  }
  if (name == "DateTimeFormatPart" || name == "NumberFormatPart") {
    return this->dateFormatChecker->dateFormatPartType();
  }
  if (name == "RelativeTimeFormatPart") {
    return this->relativePartType();
  }
  if (name == "ResolvedDateTimeFormatOptions") {
    return this->dateFormatChecker->resolvedDateOptionsType();
  }
  if (name == "DateTimeFormatOptions") {
    return this->dateFormatChecker->dateOptionsType();
  }
  if (name == "LDMLPluralRule" || name == "RelativeTimeFormatUnit" ||
      name == "UnicodeBCP47LocaleIdentifier" || name == "BCP47LanguageTag") {
    return Type::string();
  }
  for (const IntlService &service : IntlCheckHelpers::services()) {
    if (name == "Resolved" + service.name.str() + "Options") {
      return this->resolvedIntlOptionsType(service.kind);
    }
    if (name != service.name.str() + "Options") {
      continue;
    }
    auto fields = std::vector<Field>();
    for (const IntlOption &option : service.options) {
      const Type *type = option.kind == 's'   ? Type::string()
                         : option.kind == 'n' ? Type::doubleTy()
                         : option.kind == 'b' ? Type::boolTy()
                                              : Type::unionOf({Type::string(), Type::boolTy()});
      if (option.kind == 'n') {
        type = this->numberOptionType(option.name);
      }
      if ((option.kind == 's' || option.kind == 'g') && !option.values.empty()) {
        auto members = std::vector<const Type *>();
        for (llvm::StringLiteral value : option.values) {
          members.push_back(Type::stringLiteral(ToUtf16::toUtf16(value)));
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
                                IntlCreation creation) {
  bool withNew = creation == IntlCreation::NEW;
  auto failed = [&] {
    for (Expr *&arg : args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  const auto *service = this->serviceNamed(name);
  if (service == nullptr) {
    auto message = ErrorTextIntlNew::unknown(name);
    this->state.getDiagnostics().error(loc, message);
    return failed();
  }
  if (!withNew && !service->callable) {
    auto message = ErrorTextIntlNew::withNew(name);
    this->state.getDiagnostics().error(loc, message);
    return failed();
  }
  if (args.size() > 2) {
    auto message = ErrorTextDateFormat::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    return failed();
  }
  if (service->kind == IntlKind::DisplayNames && args.size() < 2) {
    auto message = ErrorTextIntlNew::noType();
    this->state.getDiagnostics().error(loc, message);
    return failed();
  }
  auto what = ErrorTextIntlNew::label(name);
  if (!this->intlOptionsChecker->checkIntlArgs(args, 0, service->kind, what)) {
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  std::vector<Expr *> all{this->kindArg(this->state.getAst(), loc, service->runtimeKind)};
  all.insert(all.end(), args.begin(), args.end());

  return this->processChecker->builtinCall(loc, this->state.getAst().make<NameExpr>(loc, "Intl"),
                                           Builtin::IntlNew, std::move(all),
                                           Type::intl(service->kind));
}

IntlChecker::ResultLanguageTags IntlChecker::languageTags(const Expr *arg, llvm::StringRef what,
                                                          bool constructor) {
  auto tags = this->literalTypes->knownTexts(arg);
  if (!tags) {
    auto message = ErrorTextLanguageTag::unknown(constructor, what);

    return err(IntlCheckerLanguageTagsError{
        .code = IntlCheckerLanguageTagsErrorCode::UNKNOWN_TAGS,
        .message = message,
    });
  }
  for (const std::string &tag : *tags) {
    if (!this->dateFormatChecker->isLanguageTag(tag)) {
      auto message = ErrorTextLanguageTag::notTag(tag);

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
  auto *locale = this->state.getLibraryDeclarations().findLibraryClass("Locale");
  if (locale != nullptr && !type->isOptional() && this->classDeclarer->classOf(type) == locale) {
    auto *tag = this->state.getAst().make<MemberExpr>(arg->loc(), arg, "tag");
    tag->type = Type::string();
    arg = tag;
    return true;
  }
  if (!this->isStringLike(type->nonOptional())) {
    auto message = ErrorTextLanguageTag::takes(what, type->name(), type->isArray());
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  auto tags = this->languageTags(arg, what, false);
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
  bool isArray = isa<ArrayLitExpr>(arg);
  arg = this->exprChecker->checkExpr(arg, isArray ? Type::array(Type::string()) : nullptr);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isArray()) {
    this->checkLocaleArg(arg, what);
    return arg;
  }
  if (type != Type::array(Type::string())) {
    auto message = ErrorTextLanguageTag::list(what, type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  auto *list = dyn_cast<ArrayLitExpr>(this->unconverted(arg));
  if (list == nullptr) {
    auto message = ErrorTextLanguageTag::listOpen(what);
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread) {
      auto message = ErrorTextLanguageTag::spread(what);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      return arg;
    }
    auto tags = this->languageTags(element.expr, what, false);
    if (tags.isErr()) {
      this->state.getDiagnostics().error(element.expr->loc(), tags.error().message);
      return arg;
    }
  }

  return arg;
}

Expr *IntlChecker::checkLocaleNew(NewExpr *expr, const Type *expected) {
  if (this->topLevelChecker->lookupValue(LibraryModuleNames::kIntlLocaleName).cls() == nullptr) {
    auto message = ErrorTextIntlLocale::missing();
    this->state.getDiagnostics().error(expr->loc(), message);
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return expr;
  }
  if (expr->args.size() == 2) {
    auto message = ErrorTextIntlLocale::options();
    this->state.getDiagnostics().error(expr->args[1]->loc(), message);
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return expr;
  }
  expr->name = LibraryModuleNames::kIntlLocaleName;
  expr->member = "";
  auto *checked = this->newChecker->checkNew(expr, expected);
  auto *made = dyn_cast<NewExpr>(checked);
  std::vector<Expr *> *args =
      made != nullptr ? (made->ctor() != nullptr ? &made->ctor()->args : &made->args) : nullptr;
  if (args != nullptr && args->size() == 1 && args->front()->type &&
      !args->front()->type->isError()) {
    auto what = ErrorTextIntlLocale::label();
    auto tags = this->languageTags(args->front(), what, true);
    if (tags.isErr()) {
      this->state.getDiagnostics().error(args->front()->loc(), tags.error().message);
    }
  }

  return checked;
}

Expr *IntlChecker::checkLocaleStatic(CallExpr *call, MemberExpr *member, const Type *expected) {
  if (this->topLevelChecker->lookupValue(LibraryModuleNames::kIntlLocaleName).cls() == nullptr) {
    auto message = ErrorTextIntlLocale::missing();
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
    auto arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      auto what = ErrorTextIntlFunction::canonical();
      call->args[0] = this->checkLocaleList(call->args[0], what);
    }
    call->target = BuiltinCall{.builtin = Builtin::IntlCanonicalLocales};
    call->type = Type::array(Type::string());
    return call;
  }
  if (member->name == "supportedValuesOf") {
    auto message = ErrorTextIntlFunction::values();
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  if (member->name == "Locale") {
    auto message = ErrorTextIntlLocale::withNew();
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }

  return this->checkIntlNew(member->loc(), member->name, call->args, IntlCreation::CALL);
}

Expr *IntlChecker::checkIntlStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto *service = cast<MemberExpr>(member->object);
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto what = ErrorTextIntlFunction::staticOf(service->name, member->name);
  if ((service->name != "DateTimeFormat" && this->serviceNamed(service->name) == nullptr) ||
      member->name != "supportedLocalesOf") {
    auto message = ErrorTextIntlFunction::onlyLocale(what);
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
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
                                            member->loc(), ToUtf16::toUtf16(what)));
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
  if (this->state.getFunctionStack().getExpectedCallback() != nullptr &&
      !this->state.getFunctionStack().getExpectedCallback()->params.empty()) {
    wantedParam = this->state.getFunctionStack().getExpectedCallback()->params[0];
    wantedRet = this->state.getFunctionStack().getExpectedCallback()->ret;
  } else if (expected != nullptr && expected->nonOptional()->isFunction()) {
    const Type *functionType = expected->nonOptional();
    wantedParam = functionType->params().empty() ? nullptr : functionType->params()[0];
    wantedRet = functionType->ret();
  }
  const Type *functionType = nullptr;
  if (kind == IntlKind::NumberFormat && name == "format") {
    const auto *param = Type::doubleTy();
    if (wantedParam != nullptr && (wantedParam->isNumeric() || wantedParam->isDecimal())) {
      param = wantedParam;
    }
    functionType = Type::function({param}, 1, false, Type::string());
  } else if (kind == IntlKind::DateTimeFormat && name == "format") {
    const auto *param = Type::date();
    if (wantedParam != nullptr && wantedParam->isNumeric()) {
      param = wantedParam;
    }
    functionType = Type::function({param}, 1, false, Type::string());
  } else if (kind == IntlKind::Collator && name == "compare") {
    const Type *returnType =
        wantedRet != nullptr && wantedRet->isFloat() ? Type::doubleTy() : Type::intTy();
    functionType = Type::function({Type::string(), Type::string()}, 2, false, returnType);
  }
  if (functionType == nullptr) {
    std::string message = kind == IntlKind::DateTimeFormat
                              ? ErrorTextIntlMember::dateFormat(name)
                              : ErrorTextIntlMember::onlyCalled(name, member->object->type->name());
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  member->type = functionType;

  return this->processChecker->builtinCall(member->loc(), member, Builtin::IntlBound, {},
                                           functionType);
}

Expr *IntlChecker::segmentsArray(Expr *segments) {
  auto *member = this->state.getAst().make<MemberExpr>(segments->loc(), segments, "%segments");
  const auto *type = Type::array(this->segmentDataType());
  member->type = type;

  return this->processChecker->builtinCall(segments->loc(), member, Builtin::SegmentsArray, {},
                                           type);
}

}
