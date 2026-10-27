#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
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

std::optional<std::vector<std::string>> textsOf(const Type *type) {
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  std::vector<std::string> texts;
  for (const Type *m : members) {
    if (!m->isStringLiteral()) {
      return std::nullopt;
    }
    texts.push_back(utf8(m->literalText()));
  }

  return texts;
}

std::optional<std::vector<double>> numbersOf(const Type *type) {
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  std::vector<double> numbers;
  for (const Type *m : members) {
    if (!m->isLiteral() || !m->literalBase()->isNumeric()) {
      return std::nullopt;
    }
    numbers.push_back(m->literalNumber());
  }

  return numbers;
}

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
    args[0] = checker_.exprChecker().checkExpr(args[0]);
    ok = checker_.intlChecker().checkLocaleArg(args[0], methodName(kind));
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
      wanted = value->isNumeric() || numbersOf(value) ? value : Type::intTy();
    } else if (found != std::end(kOptions) && textsOf(value)) {
      wanted = value;
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
  auto propOf = [&](llvm::StringRef name) -> const Expr * {
    if (!literal) {
      return nullptr;
    }
    for (const ObjectLitExpr::Property &prop : literal->props) {
      if (!prop.spread && prop.name == name && prop.value) {
        return prop.value;
      }
    }
    return nullptr;
  };
  bool exempt = checker_.indexProofs().exempt(options->loc());
  for (const Field &field : type->fields()) {
    const Expr *given = propOf(field.name);
    llvm::SMLoc at = given ? given->loc() : options->loc();
    if (field.name == "fractionalSecondDigits") {
      if (given) {
        ok &= checker_.indexProofs().requireBetween(given, 1, 3, "fractionalSecondDigits");
        continue;
      }
      std::optional<std::vector<double>> digits = numbersOf(field.type->nonOptional());
      if (!exempt && (!digits || llvm::any_of(*digits, [](double d) { return d < 1 || d > 3; }))) {
        checker_.diags_.error(at, "fractionalSecondDigits known only when the program runs may "
                                  "be out of 1 to 3, which JavaScript would throw a RangeError "
                                  "for; give it the type '1 | 2 | 3'");
        ok = false;
      }
      continue;
    }
    auto found =
        llvm::find_if(kOptions, [&](const DateOption &o) { return o.name == field.name; });
    if (found == std::end(kOptions) || found->values.empty() || exempt) {
      continue;
    }
    std::optional<std::vector<std::string>> texts =
        given ? checker_.literalTypes().knownTexts(given) : std::nullopt;
    if (!texts) {
      texts = textsOf(field.type->nonOptional());
    }
    if (!texts) {
      std::string name = given ? valueText(given) : "value";
      checker_.diags_.error(at, "the option '" + field.name +
                                    "' known only when the program runs may be no value of it, "
                                    "which JavaScript would throw a RangeError for; check it "
                                    "first, e.g. 'if (" +
                                    name + " == \"" + found->values[0].str() + "\" || " + name +
                                    " == \"" + found->values[1].str() + "\")'");
      ok = false;
      continue;
    }
    for (const std::string &text : *texts) {
      if (llvm::is_contained(found->values, text)) {
        continue;
      }
      std::string values;
      for (size_t i = 0; i < found->values.size(); ++i) {
        values += (i == 0                          ? "'"
                   : i + 1 == found->values.size() ? " or '"
                                                   : ", '") +
                  found->values[i].str() + "'";
      }
      checker_.diags_.error(at, "'" + text + "' is not a value of the option '" + field.name +
                                    "', and JavaScript would throw a "
                                    "RangeError; it is " +
                                    values);
      ok = false;
      break;
    }
  }

  return ok;
}

const Type *DateFormatChecker::dateOptionsType() {
  std::vector<Field> fields;
  for (const DateOption &option : kOptions) {
    const Type *value = Type::string();
    if (!option.values.empty()) {
      std::vector<const Type *> literals;
      for (llvm::StringLiteral text : option.values) {
        literals.push_back(Type::stringLiteral(std::u16string(text.begin(), text.end())));
      }
      value = Type::unionOf(literals);
    }
    fields.push_back({option.name.str(), Type::optional(value), false});
  }
  fields.push_back({"hour12", Type::optional(Type::boolTy()), false});
  fields.push_back({"fractionalSecondDigits",
                    Type::optional(Type::unionOf({Type::numberLiteral(1, Type::intTy()),
                                                  Type::numberLiteral(2, Type::intTy()),
                                                  Type::numberLiteral(3, Type::intTy())})),
                    false});

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
