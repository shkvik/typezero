#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/new_checker/new_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "compiler/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<std::u16string> literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

const IntlService *serviceNamed(llvm::StringRef name) {
  for (const IntlService &s : services()) {
    if (s.name == name) {
      return &s;
    }
  }

  return nullptr;
}

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

const Type *relativePartType() {
  return Type::object({{"type", Type::string(), false},
                       {"value", Type::string(), false},
                       {"unit", Type::optional(Type::string()), false}},
                      "Intl.RelativeTimeFormatPart");
}

const Expr *unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool isStringLike(const Type *type) {
  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() &&
          llvm::all_of(type->members(), [](const Type *m) { return m->isStringLiteral(); }));
}

const Type *numberOptionType(llvm::StringRef name) {
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

}

static Expr *kindArg(AstContext &ctx, llvm::SMLoc loc, int kind) {
  auto *lit = ctx.make<IntLitExpr>(loc, uint64_t(kind));
  lit->type = Type::intTy();

  return lit;
}

IntlChecker::IntlChecker(Checker &checker) : checker_(checker) {}

const Type *IntlChecker::segmentDataType() {
  return Type::object({{"segment", Type::string(), false},
                       {"index", Type::intTy(), false},
                       {"input", Type::string(), false},
                       {"isWordLike", Type::optional(Type::boolTy()), false}},
                      "Intl.SegmentData");
}

const Type *IntlChecker::resolvedIntlOptionsType(IntlKind kind) {
  const Type *text = Type::string(), *maybe = Type::optional(Type::string());
  const Type *integer = Type::intTy(), *maybeInt = Type::optional(Type::intTy());
  const Type *flag = Type::boolTy();
  std::vector<Field> digits = {{"minimumIntegerDigits", integer, false},
                               {"minimumFractionDigits", maybeInt, false},
                               {"maximumFractionDigits", maybeInt, false},
                               {"minimumSignificantDigits", maybeInt, false},
                               {"maximumSignificantDigits", maybeInt, false}};
  std::vector<Field> rounding = {{"roundingIncrement", integer, false},
                                 {"roundingMode", text, false},
                                 {"roundingPriority", text, false},
                                 {"trailingZeroDisplay", text, false}};
  std::vector<Field> fields = {{"locale", text, false}};
  const char *name = "";
  switch (kind) {
  case IntlKind::NumberFormat:
    name = "Intl.ResolvedNumberFormatOptions";
    fields.insert(fields.end(), {{"numberingSystem", text, false},
                                 {"style", text, false},
                                 {"currency", maybe, false},
                                 {"currencyDisplay", maybe, false},
                                 {"currencySign", maybe, false},
                                 {"unit", maybe, false},
                                 {"unitDisplay", maybe, false}});
    fields.insert(fields.end(), digits.begin(), digits.end());
    fields.insert(fields.end(),
                  {{"useGrouping", Type::unionOf({Type::string(), Type::boolTy()}), false},
                   {"notation", text, false},
                   {"compactDisplay", maybe, false},
                   {"signDisplay", text, false}});
    fields.insert(fields.end(), rounding.begin(), rounding.end());
    break;
  case IntlKind::Collator:
    name = "Intl.ResolvedCollatorOptions";
    fields.insert(fields.end(), {{"usage", text, false},
                                 {"sensitivity", text, false},
                                 {"ignorePunctuation", flag, false},
                                 {"collation", text, false},
                                 {"numeric", flag, false},
                                 {"caseFirst", text, false}});
    break;
  case IntlKind::PluralRules:
    name = "Intl.ResolvedPluralRulesOptions";
    fields.push_back({"type", text, false});
    fields.insert(fields.end(), digits.begin(), digits.end());
    fields.push_back({"pluralCategories", Type::array(Type::string()), false});
    fields.insert(fields.end(), rounding.begin(), rounding.end());
    break;
  case IntlKind::RelativeTimeFormat:
    name = "Intl.ResolvedRelativeTimeFormatOptions";
    fields.insert(
        fields.end(),
        {{"style", text, false}, {"numeric", text, false}, {"numberingSystem", text, false}});
    break;
  case IntlKind::ListFormat:
    name = "Intl.ResolvedListFormatOptions";
    fields.insert(fields.end(), {{"type", text, false}, {"style", text, false}});
    break;
  case IntlKind::DisplayNames:
    name = "Intl.ResolvedDisplayNamesOptions";
    fields.insert(fields.end(), {{"style", text, false},
                                 {"type", text, false},
                                 {"fallback", text, false},
                                 {"languageDisplay", maybe, false}});
    break;
  default:
    name = "Intl.ResolvedSegmenterOptions";
    fields.push_back({"granularity", text, false});
    break;
  }

  return Type::object(std::move(fields), name);
}

const Type *IntlChecker::intlTypeNamed(llvm::StringRef name) {
  if (name == "DateTimeFormat") {
    return Type::dateFormat();
  }
  if (const IntlService *service = serviceNamed(name)) {
    return Type::intl(service->kind);
  }
  if (name == "Segments") {
    return Type::intl(IntlKind::Segments);
  }
  if (name == "SegmentData") {
    return segmentDataType();
  }
  if (name == "DateTimeFormatPart" || name == "NumberFormatPart") {
    return DateFormatChecker::dateFormatPartType();
  }
  if (name == "RelativeTimeFormatPart") {
    return relativePartType();
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
  for (const IntlService &service : services()) {
    if (name == "Resolved" + service.name.str() + "Options") {
      return resolvedIntlOptionsType(service.kind);
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
        type = numberOptionType(option.name);
      }
      if ((option.kind == 's' || option.kind == 'g') && !option.values.empty()) {
        std::vector<const Type *> members;
        for (llvm::StringLiteral value : option.values) {
          members.push_back(Type::stringLiteral(utf16(value)));
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
      arg = checker_.exprChecker().checkExpr(arg);
    }
    auto *error = checker_.ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  const IntlService *service = serviceNamed(name);
  if (!service) {
    checker_.diags_.error(loc, "'Intl." + name +
                                   "' is not supported yet; Intl has DateTimeFormat, NumberFormat, "
                                   "Collator, PluralRules, RelativeTimeFormat, ListFormat, "
                                   "DisplayNames, Segmenter and Locale");
    return failed();
  }
  if (!withNew && !service->callable) {
    checker_.diags_.error(loc, "'Intl." + name + "' is made with 'new': 'new Intl." + name +
                                   "(...)', and JavaScript would throw a TypeError without it");
    return failed();
  }
  if (args.size() > 2) {
    checker_.diags_.error(loc, "expected " + argumentCount(0, 2) + ", but got " +
                                   std::to_string(args.size()));
    return failed();
  }
  if (service->kind == IntlKind::DisplayNames && args.size() < 2) {
    checker_.diags_.error(loc, "Intl.DisplayNames needs the option 'type', e.g. 'new "
                               "Intl.DisplayNames(\"ru\", { type: \"region\" })', which JavaScript "
                               "would throw a TypeError without");
    return failed();
  }
  if (!checker_.intlOptionsChecker().checkIntlArgs(args, 0, service->kind, "Intl." + name.str())) {
    auto *error = checker_.ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  std::vector<Expr *> all{kindArg(checker_.ctx_, loc, service->runtimeKind)};
  all.insert(all.end(), args.begin(), args.end());

  return checker_.processChecker().builtinCall(loc, checker_.ctx_.make<NameExpr>(loc, "Intl"),
                                               Builtin::IntlNew, std::move(all),
                                               Type::intl(service->kind));
}

bool IntlChecker::checkTags(const Expr *arg, llvm::StringRef what, bool constructor) {
  std::optional<std::vector<std::string>> tags = checker_.literalTypes().knownTexts(arg);
  if (!tags) {
    checker_.diags_.error(
        arg->loc(),
        constructor
            ? std::string("a language known only when the program runs may be no language "
                          "tag, which JavaScript would throw a RangeError for; parse it with "
                          "'Intl.Locale.parse(tag)', which gives a Result")
            : "a language known only when the program runs may be no language tag, which "
              "JavaScript would throw a RangeError for; parse it first: "
              "'Intl.Locale.parse(tag)' gives a Result with an Intl.Locale, which " +
                  what.str() + " takes");
    return false;
  }
  for (const std::string &tag : *tags) {
    if (!isLanguageTag(tag)) {
      checker_.diags_.error(arg->loc(),
                            "'" + tag +
                                "' is not a language tag, and JavaScript would throw a "
                                "RangeError; write it as BCP 47 does, e.g. 'en-US' or 'ru'");
      return false;
    }
  }

  return true;
}

bool IntlChecker::checkLocaleArg(Expr *&arg, llvm::StringRef what) {
  const Type *type = arg->type;
  if (type->isError()) {
    return false;
  }
  if (type->isUndefined()) {
    return true;
  }
  if (ClassDecl *locale = checker_.libraryClass("Locale");
      locale && !type->isOptional() && ClassDeclarer::classOf(type) == locale) {
    auto *tag = checker_.ctx_.make<MemberExpr>(arg->loc(), arg, "#tag");
    tag->type = Type::string();
    arg = tag;
    return true;
  }
  if (!isStringLike(type->nonOptional())) {
    std::string message = what.str() +
                          " takes the language as a string, e.g. \"ru-RU\", or an Intl.Locale, "
                          "found '" +
                          type->name().str() + "'";
    if (type->isArray()) {
      message += "; pass one language";
    }
    checker_.diags_.error(arg->loc(), message);
    return false;
  }
  if (!checkTags(arg, what, false)) {
    return false;
  }
  if (!type->nonOptional()->isString()) {
    arg = checker_.typeRelations().convert(arg, type->isOptional() ? Type::optional(Type::string())
                                                                   : Type::string());
  }

  return true;
}

Expr *IntlChecker::checkLocaleList(Expr *arg, llvm::StringRef what) {
  bool array = isa<ArrayLitExpr>(arg);
  arg = checker_.exprChecker().checkExpr(arg, array ? Type::array(Type::string()) : nullptr);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isArray()) {
    checkLocaleArg(arg, what);
    return arg;
  }
  if (type != Type::array(Type::string())) {
    checker_.diags_.error(arg->loc(), what.str() +
                                          " takes a language or an array of them, found '" +
                                          type->name().str() + "'");
    return arg;
  }
  auto *list = dyn_cast<ArrayLitExpr>(unconverted(arg));
  if (!list) {
    checker_.diags_.error(arg->loc(),
                          "languages known only when the program runs may be no language tags, "
                          "which JavaScript would throw a RangeError for; parse each with "
                          "'Intl.Locale.parse(tag)', which gives a Result, and pass them to " +
                              what.str() + " one by one");
    return arg;
  }
  for (const ArrayLitExpr::Element &e : list->elements) {
    if (e.spread) {
      checker_.diags_.error(e.expr->loc(),
                            what.str() + " takes the languages written in the array, not spread");
      return arg;
    }
    if (!checkTags(e.expr, what, false)) {
      return arg;
    }
  }

  return arg;
}

Expr *IntlChecker::checkLocaleNew(NewExpr *expr, const Type *expected) {
  if (!checker_.topLevelChecker().lookupValue(kIntlLocaleName).cls) {
    checker_.diags_.error(expr->loc(), "'Intl.Locale' is not available here");
    for (Expr *&arg : expr->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  if (expr->args.size() == 2) {
    checker_.diags_.error(expr->args[1]->loc(),
                          "the options of Intl.Locale are not supported; write them into the "
                          "tag, e.g. 'new Intl.Locale(\"th-TH-u-ca-buddhist\")'");
    for (Expr *&arg : expr->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  expr->name = kIntlLocaleName;
  expr->member = "";
  Expr *checked = checker_.newChecker().checkNew(expr, expected);
  auto *made = dyn_cast<NewExpr>(checked);
  std::vector<Expr *> *args = made ? (made->ctor ? &made->ctor->args : &made->args) : nullptr;
  if (args && args->size() == 1 && args->front()->type && !args->front()->type->isError()) {
    checkTags(args->front(), "Intl.Locale", true);
  }

  return checked;
}

Expr *IntlChecker::checkLocaleStatic(CallExpr *call, MemberExpr *member, const Type *expected) {
  if (!checker_.topLevelChecker().lookupValue(kIntlLocaleName).cls) {
    checker_.diags_.error(member->loc(), "'Intl.Locale' is not available here");
    call->type = Type::error();
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  member->object = checker_.ctx_.make<NameExpr>(member->object->loc(), kIntlLocaleName);

  return checker_.callChecker().checkCall(call, expected);
}

Expr *IntlChecker::checkIntlFunction(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (member->name == "getCanonicalLocales") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = checkLocaleList(call->args[0], "Intl.getCanonicalLocales");
    }
    call->builtin = Builtin::IntlCanonicalLocales;
    call->type = Type::array(Type::string());
    return call;
  }
  if (member->name == "supportedValuesOf") {
    checker_.diags_.error(member->loc(), "'Intl.supportedValuesOf' is not supported yet");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (member->name == "Locale") {
    checker_.diags_.error(member->loc(), "'Intl.Locale' is made with 'new': 'new Intl.Locale(tag)' "
                                         "or 'Intl.Locale.parse(tag)', and JavaScript would throw "
                                         "a TypeError without it");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }

  return checkIntlNew(member->loc(), member->name, call->args, false);
}

Expr *IntlChecker::checkIntlStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto *service = cast<MemberExpr>(member->object);
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  std::string what = "Intl." + service->name.str() + "." + member->name.str();
  if ((service->name != "DateTimeFormat" && !serviceNamed(service->name)) ||
      member->name != "supportedLocalesOf") {
    checker_.diags_.error(member->loc(), "'" + what +
                                             "' is not supported; of the classes of Intl, only "
                                             "'supportedLocalesOf' is");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
    return call;
  }
  call->args[0] = checkLocaleList(call->args[0], what);
  if (call->args[0]->type->isError()) {
    return call;
  }
  if (call->args.size() == 2) {
    std::vector<Expr *> pair{checker_.ctx_.make<UndefinedExpr>(member->loc()), call->args[1]};
    if (!checker_.intlOptionsChecker().checkIntlArgs(pair, 0, IntlKind::None, what)) {
      return call;
    }
    call->args[1] = pair[1];
  }
  call->args.insert(call->args.begin(),
                    checker_.ctx_.make<StringLitExpr>(member->loc(), utf16(what)));
  call->args[0]->type = Type::string();
  call->builtin = Builtin::IntlSupportedLocales;
  call->type = Type::array(Type::string());

  return call;
}

Expr *IntlChecker::checkIntlMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  IntlKind kind = member->object->type->intlKind();
  llvm::StringRef name = member->name;
  const Type *wantedParam = nullptr, *wantedRet = nullptr;
  if (checker_.callback_ && !checker_.callback_->params.empty()) {
    wantedParam = checker_.callback_->params[0];
    wantedRet = checker_.callback_->ret;
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
    if (kind == IntlKind::DateTimeFormat) {
      checker_.diags_.error(member->loc(),
                            "property '" + name +
                                "' of Intl.DateTimeFormat can only be called: 'format(d)', "
                                "'formatToParts(d)' or 'resolvedOptions()'");
    } else {
      checker_.diags_.error(member->loc(),
                            "property '" + name + "' of " + member->object->type->name().str() +
                                " can only be called, '" + name +
                                "(...)'; as values there are only 'format' of "
                                "Intl.NumberFormat and Intl.DateTimeFormat and 'compare' "
                                "of Intl.Collator");
    }
    return member;
  }
  member->type = fnType;

  return checker_.processChecker().builtinCall(member->loc(), member, Builtin::IntlBound, {},
                                               fnType);
}

Expr *IntlChecker::segmentsArray(Expr *segments) {
  auto *member = checker_.ctx_.make<MemberExpr>(segments->loc(), segments, "%segments");
  const Type *type = Type::array(segmentDataType());
  member->type = type;

  return checker_.processChecker().builtinCall(segments->loc(), member, Builtin::SegmentsArray, {},
                                               type);
}

}
