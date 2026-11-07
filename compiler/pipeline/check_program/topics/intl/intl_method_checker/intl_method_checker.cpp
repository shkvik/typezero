#include "compiler/pipeline/check_program/topics/intl/intl_method_checker/intl_method_checker.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/topics/date/date_format_checker/date_format_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/intl/intl_checker/intl_checker.h"
#include "compiler/pipeline/check_program/topics/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/pipeline/check_program/topics/math/math_checker/math_checker.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

std::string IntlMethodChecker::utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<std::u16string> IntlMethodChecker::literalText(const Expr *e) {
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return tmpl->quasis[0];
  }

  return std::nullopt;
}

const Type *IntlMethodChecker::relativePartType() {
  return Type::object({{"type", Type::string(), false},
                       {"value", Type::string(), false},
                       {"unit", Type::optional(Type::string()), false}},
                      "Intl.RelativeTimeFormatPart");
}

std::string IntlMethodChecker::closedArgProblem(
    const Expr *arg, const std::optional<std::vector<std::string>> &texts,
    llvm::ArrayRef<llvm::StringLiteral> allowed, llvm::StringRef what) {
  if (!arg->type || arg->type->isError()) {
    return "";
  }
  if (!texts) {
    return "a string known only when the program runs may be no " + what.str() +
           ", which JavaScript would throw a RangeError for; compare it with them first, "
           "e.g. 'if (x == \"" +
           allowed[0].str() + "\" || x == \"" + allowed[1].str() + "\")'";
  }
  for (const std::string &text : *texts) {
    if (!llvm::is_contained(allowed, text)) {
      return "'" + text + "' is no " + what.str() + ", and JavaScript would throw a RangeError";
    }
  }

  return "";
}

IntlMethodChecker::IntlMethodChecker(CheckState &owner)
    : state(owner) {}

Expr *IntlMethodChecker::checkIntlNumber(Expr *arg, llvm::StringRef method) {
  arg = GenericChecker(this->state).apparent(ExprChecker(this->state).checkExpr(arg));
  const Type *type = arg->type;
  if (type->isNumeric() || type->isDecimal() || type->isError()) {
    return arg;
  }
  std::string message =
      "'" + method.str() + "' takes a number or a decimal, found '" + type->name().str() + "'";
  if (type->isOptional() && (type->inner()->isNumeric() || type->inner()->isDecimal())) {
    message += "; the value may be undefined: check it first";
  } else if (type->isString()) {
    message += "; read the number first, e.g. 'decimal(s)', which keeps every digit";
  }
  this->state.getDiagnostics().error(arg->loc(), message);

  return arg;
}

Expr *IntlMethodChecker::checkIntlMethod(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  IntlKind kind = member->object->type->intlKind();
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }

    return call;
  };
  auto done = [&](Builtin builtin, const Type *type) {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = type;

    return call;
  };
  std::string service = member->object->type->name().str();
  if (name == "resolvedOptions" && kind != IntlKind::Segments) {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    return done(Builtin::IntlResolvedOptions, IntlChecker::resolvedIntlOptionsType(kind));
  }
  switch (kind) {
  case IntlKind::NumberFormat:
    if (name == "format" || name == "formatToParts") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      call->args[0] = this->checkIntlNumber(call->args[0], name);
      return name == "format" ? done(Builtin::NfFormat, Type::string())
                              : done(Builtin::NfFormatToParts,
                                     Type::array(DateFormatChecker::dateFormatPartType()));
    }
    if (name == "formatRange" || name == "formatRangeToParts") {
      this->state.getDiagnostics().error(
          member->loc(), "'" + name + "' is not supported yet; format both numbers");
      return loosely();
    }
    break;
  case IntlKind::Collator:
    if (name == "compare") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 2, 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      for (Expr *&arg : call->args) {
        arg = ArgumentChecker(this->state).checkArg(arg, Type::string());
      }
      return done(Builtin::CollatorCompare, Type::intTy());
    }
    break;
  case IntlKind::PluralRules:
    if (name == "select") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      call->args[0] = this->checkIntlNumber(call->args[0], name);
      return done(Builtin::PrSelect, Type::string());
    }
    if (name == "selectRange") {
      this->state.getDiagnostics().error(member->loc(),
                                         "'selectRange' is not supported yet; select each number");
      return loosely();
    }
    break;
  case IntlKind::RelativeTimeFormat:
    if (name == "format" || name == "formatToParts") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 2, 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      call->args[0] = MathChecker(this->state).numericArg(call->args[0]);
      if (call->args[0]->type->isNumeric()) {
        call->args[0] = TypeRelations(this->state).convert(call->args[0], Type::doubleTy());
      }
      std::optional<std::u16string> unit = IntlMethodChecker::literalText(call->args[1]);
      call->args[1] = ArgumentChecker(this->state).checkArg(call->args[1], Type::string());
      static const llvm::StringLiteral units[] = {
          "year", "years", "quarter", "quarters", "month",  "months",  "week",   "weeks",
          "day",  "days",  "hour",    "hours",    "minute", "minutes", "second", "seconds"};
      if (unit && !llvm::is_contained(units, IntlMethodChecker::utf8(*unit))) {
        this->state.getDiagnostics().error(
            call->args[1]->loc(),
            "'" + IntlMethodChecker::utf8(*unit) +
                "' is not a unit of Intl.RelativeTimeFormat, and JavaScript would throw "
                "a RangeError; the units are year, quarter, month, week, day, hour, "
                "minute and second, also in the plural");
      } else if (std::string problem =
                     unit ? ""
                          : IntlMethodChecker::closedArgProblem(
                                call->args[1], LiteralTypes(this->state).knownTexts(call->args[1]),
                                units, "unit of time");
                 !problem.empty()) {
        this->state.getDiagnostics().error(call->args[1]->loc(), problem);
      }
      return name == "format" ? done(Builtin::RtfFormat, Type::string())
                              : done(Builtin::RtfFormatToParts,
                                     Type::array(IntlMethodChecker::relativePartType()));
    }
    break;
  case IntlKind::ListFormat:
    if (name == "format" || name == "formatToParts") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      call->args[0] =
          ArgumentChecker(this->state).checkArg(call->args[0], Type::array(Type::string()));
      return name == "format" ? done(Builtin::LfFormat, Type::string())
                              : done(Builtin::LfFormatToParts,
                                     Type::array(DateFormatChecker::dateFormatPartType()));
    }
    break;
  case IntlKind::DisplayNames:
    if (name == "of") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
      return done(Builtin::DnOf, Type::optional(Type::string()));
    }
    break;
  case IntlKind::Segmenter:
    if (name == "segment") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
      return done(Builtin::SegSegment, Type::intl(IntlKind::Segments));
    }
    break;
  case IntlKind::Segments:
    if (name == "containing") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 0, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arity.error().message);
        return call;
      }
      if (!call->args.empty()) {
        call->args[0] = MathChecker(this->state).numericArg(call->args[0]);
        if (call->args[0]->type->isNumeric()) {
          call->args[0] = TypeRelations(this->state).convert(call->args[0], Type::doubleTy());
        }
      }
      return done(Builtin::SegmentsContaining, Type::optional(IntlChecker::segmentDataType()));
    }
    break;
  default:
    break;
  }
  this->state.getDiagnostics().error(
      member->loc(), "property '" + name + "' does not exist on type '" + service + "'");

  return loosely();
}

Expr *IntlMethodChecker::checkNumberLocale(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 0, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  if (!IntlOptionsChecker(this->state)
           .checkIntlArgs(call->args, 0, IntlKind::NumberFormat, "toLocaleString")) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::NumToLocale};
  call->type = Type::string();

  return call;
}

Expr *IntlMethodChecker::checkStringLocale(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  if (member->name == "normalize") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      std::optional<std::u16string> form = IntlMethodChecker::literalText(call->args[0]);
      static const llvm::StringLiteral forms[] = {"NFC", "NFD", "NFKC", "NFKD"};
      call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
      if (form && !llvm::is_contained(forms, IntlMethodChecker::utf8(*form))) {
        this->state.getDiagnostics().error(
            call->args[0]->loc(), "'" + IntlMethodChecker::utf8(*form) +
                                      "' is not a normalization form, and JavaScript would throw a "
                                      "RangeError; it is 'NFC', 'NFD', 'NFKC' or 'NFKD'");
      } else if (std::string problem =
                     form ? ""
                          : IntlMethodChecker::closedArgProblem(
                                call->args[0], LiteralTypes(this->state).knownTexts(call->args[0]),
                                forms, "normalization form");
                 !problem.empty()) {
        this->state.getDiagnostics().error(call->args[0]->loc(), problem);
      }
    }
    call->target = BuiltinCall{.builtin = Builtin::StrNormalize};
    call->type = Type::string();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
  std::vector<Expr *> rest(call->args.begin() + 1, call->args.end());
  if (!IntlOptionsChecker(this->state)
           .checkIntlArgs(rest, 0, IntlKind::Collator, "localeCompare")) {
    return call;
  }
  std::copy(rest.begin(), rest.end(), call->args.begin() + 1);
  call->target = BuiltinCall{.builtin = Builtin::StrLocaleCompare};
  call->type = Type::intTy();

  return call;
}

}
