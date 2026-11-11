#include "compiler/semantic/library/intl/intl_method_checker/intl_method_checker.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

IntlMethodChecker::IntlMethodChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->mathChecker = &all.mathChecker;
  this->literalTypes = &all.literalTypes;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->argumentChecker = &all.argumentChecker;
  this->intlOptionsChecker = &all.intlOptionsChecker;
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

IntlMethodChecker::ResultIntlNumber IntlMethodChecker::intlNumber(const Type *type,
                                                                  llvm::StringRef method) {
  if (!type->isNumeric() && !type->isDecimal() && !type->isError()) {
    std::string message = ErrorTextIntlNumber::notNumber(method, type);

    return err(IntlMethodCheckerIntlNumberError{
        .code = IntlMethodCheckerIntlNumberErrorCode::NOT_A_NUMBER,
        .message = message,
    });
  }

  return ok(type);
}

IntlMethodChecker::ResultClosedText
IntlMethodChecker::closedArg(const Expr *arg, const std::optional<std::vector<std::string>> &texts,
                             llvm::ArrayRef<llvm::StringLiteral> allowed, llvm::StringRef what) {
  if (!arg->type || arg->type->isError()) {
    return ok(arg);
  }
  if (!texts) {
    std::string message = ErrorTextClosedArg::unknown(what, allowed[0], allowed[1]);

    return err(IntlMethodCheckerClosedArgError{
        .code = IntlMethodCheckerClosedArgErrorCode::UNKNOWN_TEXT,
        .message = message,
    });
  }
  for (const std::string &text : *texts) {
    if (!llvm::is_contained(allowed, text)) {
      std::string message = ErrorTextClosedArg::notAllowed(text, what);

      return err(IntlMethodCheckerClosedArgError{
          .code = IntlMethodCheckerClosedArgErrorCode::NOT_ALLOWED,
          .message = message,
      });
    }
  }

  return ok(arg);
}

Expr *IntlMethodChecker::checkIntlNumber(Expr *arg, llvm::StringRef method) {
  arg = this->genericChecker->apparent(this->exprChecker->checkExpr(arg));
  ResultIntlNumber number = IntlMethodChecker::intlNumber(arg->type, method);
  if (number.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), number.error().message);
  }

  return arg;
}

Expr *IntlMethodChecker::checkIntlMethod(CallExpr *call, MemberExpr *member) {
  IntlKind kind = member->object->type->intlKind();
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  const IntlMethodInfo *found = IntlMethodTable::find(kind, member->name);
  if (found == nullptr) {
    std::string service = member->object->type->name().str();
    std::string message = ErrorTextIntlMethod::noMember(member->name, service);
    this->state.getDiagnostics().error(member->loc(), message);
    return this->checkLoosely(call);
  }

  switch (found->method) {
    case IntlMethod::RESOLVED_OPTIONS:
      return this->checkResolvedOptions(call, member, kind);
    case IntlMethod::NUMBER_FORMAT:
      return this->checkNumberFormat(call, member, false);
    case IntlMethod::NUMBER_FORMAT_TO_PARTS:
      return this->checkNumberFormat(call, member, true);
    case IntlMethod::NUMBER_FORMAT_RANGE:
      return this->reportUnsupported(call, member, ErrorTextIntlMethod::range(member->name));
    case IntlMethod::COLLATOR_COMPARE:
      return this->checkCollatorCompare(call, member);
    case IntlMethod::PLURAL_SELECT:
      return this->checkPluralSelect(call, member);
    case IntlMethod::PLURAL_SELECT_RANGE:
      return this->reportUnsupported(call, member, ErrorTextIntlMethod::selectSpan());
    case IntlMethod::RELATIVE_FORMAT:
      return this->checkRelativeFormat(call, member, false);
    case IntlMethod::RELATIVE_FORMAT_TO_PARTS:
      return this->checkRelativeFormat(call, member, true);
    case IntlMethod::LIST_FORMAT:
      return this->checkListFormat(call, member, false);
    case IntlMethod::LIST_FORMAT_TO_PARTS:
      return this->checkListFormat(call, member, true);
    case IntlMethod::DISPLAY_NAME_OF:
      return this->checkDisplayNameOf(call, member);
    case IntlMethod::SEGMENTER_SEGMENT:
      return this->checkSegment(call, member);
    case IntlMethod::SEGMENTS_CONTAINING:
      return this->checkSegmentsContaining(call, member);
  }

  return call;
}

Expr *IntlMethodChecker::checkLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *IntlMethodChecker::toBuiltinCall(CallExpr *call, Builtin builtin, const Type *type) {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;

  return call;
}

Expr *IntlMethodChecker::reportUnsupported(CallExpr *call, MemberExpr *member,
                                           const std::string &message) {
  this->state.getDiagnostics().error(member->loc(), message);

  return this->checkLoosely(call);
}

Expr *IntlMethodChecker::checkResolvedOptions(CallExpr *call, MemberExpr *member, IntlKind kind) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  const Type *options = IntlChecker::resolvedIntlOptionsType(kind);

  return IntlMethodChecker::toBuiltinCall(call, Builtin::IntlResolvedOptions, options);
}

Expr *IntlMethodChecker::checkNumberFormat(CallExpr *call, MemberExpr *member, bool toParts) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->checkIntlNumber(call->args[0], member->name);
  if (!toParts) {
    return IntlMethodChecker::toBuiltinCall(call, Builtin::NfFormat, Type::string());
  }
  const Type *parts = Type::array(DateFormatChecker::dateFormatPartType());

  return IntlMethodChecker::toBuiltinCall(call, Builtin::NfFormatToParts, parts);
}

Expr *IntlMethodChecker::checkCollatorCompare(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  for (Expr *&arg : call->args) {
    arg = this->argumentChecker->checkArg(arg, Type::string());
  }

  return IntlMethodChecker::toBuiltinCall(call, Builtin::CollatorCompare, Type::intTy());
}

Expr *IntlMethodChecker::checkPluralSelect(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->checkIntlNumber(call->args[0], member->name);

  return IntlMethodChecker::toBuiltinCall(call, Builtin::PrSelect, Type::string());
}

Expr *IntlMethodChecker::checkRelativeFormat(CallExpr *call, MemberExpr *member, bool toParts) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->mathChecker->numericArg(call->args[0]);
  if (call->args[0]->type->isNumeric()) {
    call->args[0] = this->typeRelations->convert(call->args[0], Type::doubleTy());
  }
  this->walkRelativeUnit(call);
  if (!toParts) {
    return IntlMethodChecker::toBuiltinCall(call, Builtin::RtfFormat, Type::string());
  }
  const Type *parts = Type::array(IntlMethodChecker::relativePartType());

  return IntlMethodChecker::toBuiltinCall(call, Builtin::RtfFormatToParts, parts);
}

void IntlMethodChecker::walkRelativeUnit(CallExpr *call) {
  std::optional<std::u16string> unit = IntlMethodChecker::literalText(call->args[1]);
  call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::string());
  static const llvm::StringLiteral units[] = {
      "year", "years", "quarter", "quarters", "month",  "months",  "week",   "weeks",
      "day",  "days",  "hour",    "hours",    "minute", "minutes", "second", "seconds"};
  if (unit.has_value()) {
    std::string written = ToUtf8::toUtf8(*unit);
    if (!llvm::is_contained(units, written)) {
      std::string message = ErrorTextIntlMethod::badUnit(written);
      this->state.getDiagnostics().error(call->args[1]->loc(), message);
    }
    return;
  }
  std::optional<std::vector<std::string>> texts = this->literalTypes->knownTexts(call->args[1]);
  std::string what = ErrorTextClosedArg::unit();
  ResultClosedText closed = IntlMethodChecker::closedArg(call->args[1], texts, units, what);
  if (closed.isErr()) {
    this->state.getDiagnostics().error(call->args[1]->loc(), closed.error().message);
  }
}

Expr *IntlMethodChecker::checkListFormat(CallExpr *call, MemberExpr *member, bool toParts) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::array(Type::string()));
  if (!toParts) {
    return IntlMethodChecker::toBuiltinCall(call, Builtin::LfFormat, Type::string());
  }
  const Type *parts = Type::array(DateFormatChecker::dateFormatPartType());

  return IntlMethodChecker::toBuiltinCall(call, Builtin::LfFormatToParts, parts);
}

Expr *IntlMethodChecker::checkDisplayNameOf(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  const Type *name = Type::optional(Type::string());

  return IntlMethodChecker::toBuiltinCall(call, Builtin::DnOf, name);
}

Expr *IntlMethodChecker::checkSegment(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  const Type *segments = Type::intl(IntlKind::Segments);

  return IntlMethodChecker::toBuiltinCall(call, Builtin::SegSegment, segments);
}

Expr *IntlMethodChecker::checkSegmentsContaining(CallExpr *call, MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  if (!call->args.empty()) {
    call->args[0] = this->mathChecker->numericArg(call->args[0]);
    if (call->args[0]->type->isNumeric()) {
      call->args[0] = this->typeRelations->convert(call->args[0], Type::doubleTy());
    }
  }
  const Type *segment = Type::optional(IntlChecker::segmentDataType());

  return IntlMethodChecker::toBuiltinCall(call, Builtin::SegmentsContaining, segment);
}

Expr *IntlMethodChecker::checkNumberLocale(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool isRejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      isRejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  if (!this->intlOptionsChecker->checkIntlArgs(call->args, 0, IntlKind::NumberFormat,
                                               "toLocaleString")) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::NumToLocale};
  call->type = Type::string();

  return call;
}

Expr *IntlMethodChecker::checkStringLocale(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  const StringMethodInfo *method = StringMethodTable::find(member->name);
  if (method != nullptr && method->method == StringMethod::NORMALIZE) {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty()) {
      std::optional<std::u16string> form = IntlMethodChecker::literalText(call->args[0]);
      static const llvm::StringLiteral forms[] = {"NFC", "NFD", "NFKC", "NFKD"};
      call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
      if (form && !llvm::is_contained(forms, ToUtf8::toUtf8(*form))) {
        std::string message = ErrorTextIntlMethod::badForm(ToUtf8::toUtf8(*form));
        this->state.getDiagnostics().error(call->args[0]->loc(), message);
      } else if (!form) {
        std::optional<std::vector<std::string>> texts =
            this->literalTypes->knownTexts(call->args[0]);
        std::string what = ErrorTextClosedArg::form();
        ResultClosedText closed = IntlMethodChecker::closedArg(call->args[0], texts, forms, what);
        if (closed.isErr()) {
          this->state.getDiagnostics().error(call->args[0]->loc(), closed.error().message);
        }
      }
    }
    call->target = BuiltinCall{.builtin = Builtin::StrNormalize};
    call->type = Type::string();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  std::vector<Expr *> rest(call->args.begin() + 1, call->args.end());
  if (!this->intlOptionsChecker->checkIntlArgs(rest, 0, IntlKind::Collator, "localeCompare")) {
    return call;
  }
  std::copy(rest.begin(), rest.end(), call->args.begin() + 1);
  call->target = BuiltinCall{.builtin = Builtin::StrLocaleCompare};
  call->type = Type::intTy();

  return call;
}

}
