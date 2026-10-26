#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "runtime/library/date/date/date.h"
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

struct DateOption {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> values;
};

const DateOption kOptions[TZ_DTO_COUNT] = {
    {"localeMatcher", {"lookup", "best fit"}},
    {"calendar", {}},
    {"numberingSystem", {}},
    {"hourCycle", {"h11", "h12", "h23", "h24"}},
    {"timeZone", {}},
    {"weekday", {"narrow", "short", "long"}},
    {"era", {"narrow", "short", "long"}},
    {"year", {"2-digit", "numeric"}},
    {"month", {"2-digit", "numeric", "narrow", "short", "long"}},
    {"day", {"2-digit", "numeric"}},
    {"dayPeriod", {"narrow", "short", "long"}},
    {"hour", {"2-digit", "numeric"}},
    {"minute", {"2-digit", "numeric"}},
    {"second", {"2-digit", "numeric"}},
    {"timeZoneName", {"short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric"}},
    {"formatMatcher", {"basic", "best fit"}},
    {"dateStyle", {"full", "long", "medium", "short"}},
    {"timeStyle", {"full", "long", "medium", "short"}},
};

const char *methodName(int kind) {
  switch (kind) {
  case TZ_DATE_LOCALE_DATE:
    return "toLocaleDateString";
  case TZ_DATE_LOCALE_TIME:
    return "toLocaleTimeString";
  case TZ_DATE_LOCALE_FORMAT:
    return "Intl.DateTimeFormat";
  default:
    return "toLocaleString";
  }
}

}

bool isLanguageTag(const std::string &tag) {
  auto alnum = [](char c) { return llvm::isAlnum(c); };
  auto alpha = [](char c) { return llvm::isAlpha(c); };
  llvm::SmallVector<llvm::StringRef, 4> parts;
  llvm::StringRef(tag).split(parts, '-');
  for (llvm::StringRef part : parts) {
    if (part.empty() || part.size() > 8 || !llvm::all_of(part, alnum)) {
      return false;
    }
  }
  llvm::StringRef language = parts[0];
  bool length = (language.size() >= 2 && language.size() <= 3) ||
                (language.size() >= 5 && language.size() <= 8);

  return length && llvm::all_of(language, alpha);
}

DateFormatChecker::DateFormatChecker(Checker &checker) : checker_(checker) {}

bool DateFormatChecker::checkLocaleArgs(std::vector<Expr *> &args, int kind) {
  bool ok = true;
  if (!args.empty()) {
    std::optional<std::u16string> tag = literalText(args[0]);
    args[0] = checker_.exprChecker().checkExpr(args[0]);
    const Type *type = args[0]->type;
    if (type->isError()) {
      ok = false;
    } else if (!type->isUndefined() && type != Type::string() &&
               type != Type::optional(Type::string())) {
      std::string message = std::string(methodName(kind)) +
                            " takes the language as a string, e.g. \"ru-RU\", found '" +
                            type->name().str() + "'";
      if (type->isArray()) {
        message += "; pass one language";
      }
      checker_.diags_.error(args[0]->loc(), message);
      ok = false;
    } else if (tag && !isLanguageTag(utf8(*tag))) {
      checker_.diags_.error(args[0]->loc(),
                            "'" + utf8(*tag) +
                                "' is not a language tag, and JavaScript would throw a "
                                "RangeError; write it as BCP 47 does, e.g. 'en-US' or 'ru'");
      ok = false;
    }
  }
  if (args.size() < 2) {
    return ok;
  }
  Expr *&options = args[1];
  options = checker_.exprChecker().checkExpr(options);
  const Type *type = options->type;
  if (type->isError()) {
    return false;
  }
  if (type->isUndefined()) {
    return ok;
  }
  if (!type->isObject() || type->isTuple() || type->isClass() || type->isRegExp() ||
      type->isMatch() || type->isDate() || type->isIntl() || type->isCrypto() || type->isBytes()) {
    checker_.diags_.error(options->loc(),
                          std::string(methodName(kind)) +
                              " takes its options as an object, e.g. '{ month: \"long\" "
                              "}', found '" +
                              type->name().str() + "'");
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  bool hasStyle = false, hasComponent = false;
  for (const Field &field : type->fields()) {
    const Type *value = field.type->nonOptional();
    auto found = llvm::find_if(kOptions, [&](const DateOption &o) { return o.name == field.name; });
    const Type *wanted = Type::string();
    if (field.name == "hour12") {
      wanted = Type::boolTy();
    } else if (field.name == "fractionalSecondDigits") {
      wanted = value->isNumeric() ? value : Type::intTy();
    } else if (found == std::end(kOptions)) {
      checker_.diags_.error(options->loc(),
                            "'" + field.name + "' is not an option of " + methodName(kind) +
                                "; the options are weekday, era, year, month, day, "
                                "dayPeriod, hour, minute, second, fractionalSecondDigits, "
                                "timeZoneName, timeZone, hour12, hourCycle, dateStyle, "
                                "timeStyle, calendar, numberingSystem, localeMatcher and "
                                "formatMatcher");
      ok = false;
      continue;
    }
    if (value != wanted) {
      checker_.diags_.error(options->loc(), "the option '" + field.name + "' is " +
                                                (wanted == Type::boolTy() ? "a boolean"
                                                 : wanted->isNumeric()    ? "a number"
                                                                          : "a string") +
                                                ", found '" + field.type->name().str() + "'");
      ok = false;
      continue;
    }
    bool present = !field.type->isOptional();
    if (field.name == "dateStyle" || field.name == "timeStyle") {
      hasStyle |= present;
      if (present && ((kind == TZ_DATE_LOCALE_DATE && field.name == "timeStyle") ||
                      (kind == TZ_DATE_LOCALE_TIME && field.name == "dateStyle"))) {
        checker_.diags_.error(options->loc(), "'" + field.name + "' is not an option of " +
                                                  methodName(kind) +
                                                  ", which JavaScript would throw a "
                                                  "TypeError for; use toLocaleString");
        ok = false;
      }
    } else if (field.name != "hour12" && field.name != "hourCycle" && field.name != "timeZone" &&
               field.name != "calendar" && field.name != "numberingSystem" &&
               field.name != "localeMatcher" && field.name != "formatMatcher") {
      hasComponent |= present;
    }
  }
  if (hasStyle && hasComponent) {
    checker_.diags_.error(options->loc(),
                          "dateStyle and timeStyle cannot go with weekday, year, month, day, "
                          "hour and the other parts, which JavaScript would throw a "
                          "TypeError for; use either the styles or the parts");
    ok = false;
  }
  if (literal) {
    for (const ObjectLitExpr::Property &prop : literal->props) {
      if (prop.spread) {
        continue;
      }
      auto found =
          llvm::find_if(kOptions, [&](const DateOption &o) { return o.name == prop.name; });
      std::optional<std::u16string> text = prop.value ? literalText(prop.value) : std::nullopt;
      if (found != std::end(kOptions) && text && !found->values.empty() &&
          !llvm::is_contained(found->values, utf8(*text))) {
        std::string values;
        for (size_t i = 0; i < found->values.size(); ++i) {
          values += (i == 0                          ? "'"
                     : i + 1 == found->values.size() ? " or '"
                                                     : ", '") +
                    found->values[i].str() + "'";
        }
        checker_.diags_.error(prop.value->loc(),
                              "'" + utf8(*text) + "' is not a value of the option '" + prop.name +
                                  "', and JavaScript would throw a "
                                  "RangeError; it is " +
                                  values);
        ok = false;
      }
      if (prop.name == "fractionalSecondDigits" && prop.value) {
        if (std::optional<int64_t> digits = checker_.constantFolder().intConstant(prop.value);
            digits && (*digits < 1 || *digits > 3)) {
          checker_.diags_.error(prop.value->loc(),
                                "fractionalSecondDigits is from 1 to 3, and JavaScript "
                                "would throw a RangeError");
          ok = false;
        }
      }
    }
  }

  return ok;
}

const Type *DateFormatChecker::dateOptionsType() {
  std::vector<Field> fields;
  for (const DateOption &option : kOptions) {
    fields.push_back({option.name.str(), Type::optional(Type::string()), false});
  }
  fields.push_back({"hour12", Type::optional(Type::boolTy()), false});
  fields.push_back({"fractionalSecondDigits", Type::optional(Type::intTy()), false});

  return Type::object(std::move(fields), "Intl.DateTimeFormatOptions");
}

const Type *DateFormatChecker::dateFormatPartType() {
  return Type::object({{"type", Type::string(), false}, {"value", Type::string(), false}},
                      "Intl.DateTimeFormatPart");
}

const Type *DateFormatChecker::resolvedDateOptionsType() {
  const Type *text = Type::string(), *maybe = Type::optional(Type::string());

  return Type::object({{"locale", text, false},
                       {"calendar", text, false},
                       {"numberingSystem", text, false},
                       {"timeZone", text, false},
                       {"hourCycle", maybe, false},
                       {"hour12", Type::optional(Type::boolTy()), false},
                       {"weekday", maybe, false},
                       {"era", maybe, false},
                       {"year", maybe, false},
                       {"month", maybe, false},
                       {"day", maybe, false},
                       {"dayPeriod", maybe, false},
                       {"hour", maybe, false},
                       {"minute", maybe, false},
                       {"second", maybe, false},
                       {"fractionalSecondDigits", Type::optional(Type::intTy()), false},
                       {"timeZoneName", maybe, false},
                       {"dateStyle", maybe, false},
                       {"timeStyle", maybe, false}},
                      "Intl.ResolvedDateTimeFormatOptions");
}

Expr *DateFormatChecker::checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  auto *callee = checker_.ctx_.make<NameExpr>(loc, "Intl");
  if (args.size() > 2) {
    checker_.diags_.error(loc, "expected " + argumentCount(0, 2) + ", but got " +
                                   std::to_string(args.size()));
    for (Expr *&arg : args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    auto *error = checker_.ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  if (!checkLocaleArgs(args, TZ_DATE_LOCALE_FORMAT)) {
    auto *error = checker_.ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }

  return checker_.processChecker().builtinCall(loc, callee, Builtin::DtfNew, args,
                                               Type::dateFormat());
}

Expr *DateFormatChecker::checkIntlCall(CallExpr *call, MemberExpr *member) {
  if (member->name == "DateTimeFormat") {
    if (checker_.callChecker().rejectSpread(call)) {
      return call;
    }
    return checkDateFormatNew(member->loc(), call->args);
  }

  return checker_.intlChecker().checkIntlFunction(call, member);
}

Expr *DateFormatChecker::checkDateFormatMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (name == "format" || name == "formatToParts") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    if (!call->args.empty()) {
      Expr *&arg = call->args[0];
      arg = checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(arg));
      if (arg->type->isNumeric()) {
        arg = checker_.typeRelations().convert(arg, Type::doubleTy());
      } else if (!arg->type->isDate() && !arg->type->isError()) {
        checker_.diags_.error(arg->loc(), "'" + name +
                                              "' takes a date or a time in milliseconds, found '" +
                                              arg->type->name() + "'");
        return call;
      }
    }
    call->builtin = name == "format" ? Builtin::DtfFormat : Builtin::DtfFormatToParts;
    call->type = name == "format" ? Type::string() : Type::array(dateFormatPartType());
    return call;
  }
  if (name == "resolvedOptions") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    call->builtin = Builtin::DtfResolvedOptions;
    call->type = resolvedDateOptionsType();
    return call;
  }
  if (name == "formatRange" || name == "formatRangeToParts") {
    checker_.diags_.error(member->loc(), "'" + name + "' is not supported yet; format both dates");
  } else {
    checker_.diags_.error(member->loc(), "property '" + name +
                                             "' does not exist on type "
                                             "'Intl.DateTimeFormat'");
  }
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

}
