#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

namespace {

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

}

std::optional<std::u16string> DateFormatChecker::literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

std::optional<std::vector<std::string>> DateFormatChecker::textsOf(const Type *type) {
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  std::vector<std::string> texts;
  for (const Type *m : members) {
    if (!m->isStringLiteral()) {
      return std::nullopt;
    }
    texts.push_back(ToUtf8::toUtf8(m->literalText()));
  }

  return texts;
}

std::optional<std::vector<double>> DateFormatChecker::numbersOf(const Type *type) {
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

const char *DateFormatChecker::methodName(int kind) {
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

bool DateFormatChecker::isLanguageTag(const std::string &tag) {
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

DateFormatChecker::ResultOptionsObject DateFormatChecker::optionsObject(const Type *type,
                                                                        int kind) {
  if (!type->isObject() || type->isTuple() || type->isClass() || type->isRegExp() ||
      type->isMatch() || type->isDate() || type->isIntl() || type->isCrypto() || type->isBytes()) {
    std::string message =
        ErrorTextDateOptions::notObject(DateFormatChecker::methodName(kind), type->name());

    return err(DateFormatCheckerOptionsObjectError{
        .code = DateFormatCheckerOptionsObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

DateFormatChecker::DateFormatChecker(SemanticState &owner)
    : state(owner) {}

bool DateFormatChecker::checkLocaleArgs(std::vector<Expr *> &args, int kind) {
  bool ok = true;
  if (!args.empty()) {
    args[0] = ExprChecker(this->state).checkExpr(args[0]);
    ok = IntlChecker(this->state).checkLocaleArg(args[0], DateFormatChecker::methodName(kind));
  }
  if (args.size() < 2) {
    return ok;
  }
  Expr *&options = args[1];
  options = ExprChecker(this->state).checkExpr(options);
  const Type *type = options->type;
  if (type->isError()) {
    return false;
  }
  if (type->isUndefined()) {
    return ok;
  }
  ResultOptionsObject object = DateFormatChecker::optionsObject(type, kind);
  if (object.isErr()) {
    this->state.getDiagnostics().error(options->loc(), object.error().message);
    return false;
  }
  auto *literal = dyn_cast<ObjectLitExpr>(options);
  bool hasStyle = false;
  bool hasComponent = false;
  for (const Field &field : type->fields()) {
    const Type *value = field.type->nonOptional();
    auto found = llvm::find_if(kOptions, [&](const DateOption &o) { return o.name == field.name; });
    const Type *wanted = Type::string();
    if (field.name == "hour12") {
      wanted = Type::boolTy();
    } else if (field.name == "fractionalSecondDigits") {
      wanted = value->isNumeric() || DateFormatChecker::numbersOf(value) ? value : Type::intTy();
    } else if (found != std::end(kOptions) && DateFormatChecker::textsOf(value)) {
      wanted = value;
    } else if (found == std::end(kOptions)) {
      std::string message =
          ErrorTextDateOptions::unknown(field.name, DateFormatChecker::methodName(kind));
      this->state.getDiagnostics().error(options->loc(), message);
      ok = false;
      continue;
    }
    if (value != wanted) {
      std::string message = ErrorTextDateOptions::kind(field.name, wanted == Type::boolTy(),
                                                       wanted->isNumeric(), field.type->name());
      this->state.getDiagnostics().error(options->loc(), message);
      ok = false;
      continue;
    }
    bool present = !field.type->isOptional();
    if (field.name == "dateStyle" || field.name == "timeStyle") {
      hasStyle |= present;
      if (present && ((kind == TZ_DATE_LOCALE_DATE && field.name == "timeStyle") ||
                      (kind == TZ_DATE_LOCALE_TIME && field.name == "dateStyle"))) {
        std::string message =
            ErrorTextDateOptions::styleOnly(field.name, DateFormatChecker::methodName(kind));
        this->state.getDiagnostics().error(options->loc(), message);
        ok = false;
      }
    } else if (field.name != "hour12" && field.name != "hourCycle" && field.name != "timeZone" &&
               field.name != "calendar" && field.name != "numberingSystem" &&
               field.name != "localeMatcher" && field.name != "formatMatcher") {
      hasComponent |= present;
    }
  }
  if (hasStyle && hasComponent) {
    std::string message = ErrorTextDateOptions::styleParts();
    this->state.getDiagnostics().error(options->loc(), message);
    ok = false;
  }
  auto propOf = [&](llvm::StringRef name) -> const Expr * {
    if (!literal) {
      return nullptr;
    }
    for (const ObjectLitExpr::Property &prop : literal->props) {
      if (!prop.spread() && prop.name == name && prop.value) {
        return prop.value;
      }
    }
    return nullptr;
  };
  bool exempt = IndexProofs(this->state).exempt(options->loc());
  for (const Field &field : type->fields()) {
    const Expr *given = propOf(field.name);
    llvm::SMLoc at = given ? given->loc() : options->loc();
    if (field.name == "fractionalSecondDigits") {
      if (given) {
        std::string digits = ErrorTextDateOptions::digitsName();
        IndexProofs::ResultBetweenProven between =
            IndexProofs(this->state).requireBetween(given, 1, 3, digits);
        if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(given->loc(), between.error().message);
        }
        ok &= between.isOk();
        continue;
      }
      std::optional<std::vector<double>> digits =
          DateFormatChecker::numbersOf(field.type->nonOptional());
      if (!exempt && (!digits || llvm::any_of(*digits, [](double d) { return d < 1 || d > 3; }))) {
        std::string message = ErrorTextDateOptions::digits();
        this->state.getDiagnostics().error(at, message);
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
        given ? LiteralTypes(this->state).knownTexts(given) : std::nullopt;
    if (!texts) {
      texts = DateFormatChecker::textsOf(field.type->nonOptional());
    }
    if (!texts) {
      std::string written = given ? BytesCheckHelpers::valueText(given) : std::string();
      std::string message =
          ErrorTextDateOptions::unknownVal(field.name, written, found->values[0], found->values[1]);
      this->state.getDiagnostics().error(at, message);
      ok = false;
      continue;
    }
    for (const std::string &text : *texts) {
      if (llvm::is_contained(found->values, text)) {
        continue;
      }
      std::string message = ErrorTextDateOptions::badValue(text, field.name, found->values);
      this->state.getDiagnostics().error(at, message);
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
  const Type *text = Type::string();
  const Type *maybe = Type::optional(Type::string());

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
  auto *callee = this->state.getAst().make<NameExpr>(loc, "Intl");
  if (args.size() > 2) {
    std::string message = ErrorTextDateFormat::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    for (Expr *&arg : args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  if (!this->checkLocaleArgs(args, TZ_DATE_LOCALE_FORMAT)) {
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }

  return ProcessChecker(this->state)
      .builtinCall(loc, callee, Builtin::DtfNew, args, Type::dateFormat());
}

Expr *DateFormatChecker::checkIntlCall(CallExpr *call, MemberExpr *member) {
  if (member->name == "DateTimeFormat") {
    if (CallChecker(this->state).rejectSpread(call)) {
      return call;
    }
    return this->checkDateFormatNew(member->loc(), call->args);
  }

  return IntlChecker(this->state).checkIntlFunction(call, member);
}

Expr *DateFormatChecker::checkDateFormatMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  if (name == "format" || name == "formatToParts") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      Expr *&arg = call->args[0];
      arg = GenericChecker(this->state).apparent(ExprChecker(this->state).checkExpr(arg));
      if (arg->type->isNumeric()) {
        arg = TypeRelations(this->state).convert(arg, Type::doubleTy());
      } else if (!arg->type->isDate() && !arg->type->isError()) {
        std::string message = ErrorTextDateFormat::formatArg(name, arg->type->name());
        this->state.getDiagnostics().error(arg->loc(), message);
        return call;
      }
    }
    call->target =
        BuiltinCall{.builtin = name == "format" ? Builtin::DtfFormat : Builtin::DtfFormatToParts};
    call->type =
        name == "format" ? Type::string() : Type::array(DateFormatChecker::dateFormatPartType());
    return call;
  }
  if (name == "resolvedOptions") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = Builtin::DtfResolvedOptions};
    call->type = DateFormatChecker::resolvedDateOptionsType();
    return call;
  }
  std::string message = name == "formatRange" || name == "formatRangeToParts"
                            ? ErrorTextDateFormat::range(name)
                            : ErrorTextDateFormat::noMember(name);
  this->state.getDiagnostics().error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }

  return call;
}

}
