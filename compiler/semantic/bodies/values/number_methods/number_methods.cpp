#include "compiler/semantic/bodies/values/number_methods/number_methods.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/number/number_method_table/number_method_table.h"
#include "compiler/program/library/number/number_static_table/number_static_table.h"
#include "compiler/program/library/string/string_static_table/string_static_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/literal_checker/literal_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/values/global_function_checker/global_function_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

constexpr int64_t kMaxCodePoint = 0x10FFFF;
constexpr int64_t kMinRadix = 2;
constexpr int64_t kMaxRadix = 36;
constexpr int64_t kMaxFixedDigits = 100;
constexpr const char *kUnnamedNumber = "x";

}

NumberMethods::NumberMethods(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->indexProofs = &all.indexProofs;
  this->mathChecker = &all.mathChecker;
  this->binaryChecker = &all.binaryChecker;
  this->concatChecker = &all.concatChecker;
  this->memberChecker = &all.memberChecker;
  this->typeRelations = &all.typeRelations;
  this->literalChecker = &all.literalChecker;
  this->argumentChecker = &all.argumentChecker;
  this->globalFunctionChecker = &all.globalFunctionChecker;
}

NumberMethods::ResultStaticCallMade NumberMethods::checkStaticCall(CallExpr *call,
                                                                   MemberExpr *member) {
  bool isString = this->callChecker->isGlobalName(member->object, GlobalName::STRING);
  call->type = Type::error();
  if (isString) {
    return this->checkStringStatic(call, member);
  }

  return this->checkNumberStatic(call, member);
}

NumberMethods::ResultStaticCallMade NumberMethods::checkStringStatic(CallExpr *call,
                                                                     MemberExpr *member) {
  auto owner = GlobalNameTable::getName(GlobalName::STRING);
  auto found = StringStaticTable::find(member->name);
  if (!found.has_value()) {
    return this->noSuchProperty(call, member->name, owner);
  }
  switch (*found) {
    case StringStatic::RAW:
      return this->checkStringRaw(call);
    case StringStatic::FROM_CHAR_CODE:
      return this->checkFromCodes(call, Builtin::FromCharCode);
    case StringStatic::FROM_CODE_POINT:
      return this->checkFromCodes(call, Builtin::FromCodePoint);
  }

  return this->noSuchProperty(call, member->name, owner);
}

NumberMethods::ResultStaticCallMade NumberMethods::checkNumberStatic(CallExpr *call,
                                                                     MemberExpr *member) {
  auto owner = GlobalNameTable::getName(GlobalName::NUMBER);
  auto found = NumberStaticTable::find(member->name);
  if (!found.has_value()) {
    return this->noSuchProperty(call, member->name, owner);
  }
  switch (*found) {
    case NumberStatic::PARSE_INT:
    case NumberStatic::PARSE_FLOAT:
      return this->checkNumberParse(call, member);
    case NumberStatic::IS_INTEGER:
      return this->checkNumberPredicate(call, Builtin::IsInteger);
    case NumberStatic::IS_SAFE_INTEGER:
      return this->checkNumberPredicate(call, Builtin::IsSafeInteger);
  }

  return this->noSuchProperty(call, member->name, owner);
}

NumberMethods::ResultStaticCallMade
NumberMethods::noSuchProperty(CallExpr *call, llvm::StringRef name, llvm::StringRef owner) {
  this->argumentChecker->walkArgsLoosely(call);
  auto message = ErrorTextNumberStatic::noProperty(name, owner);

  return err(NumberMethodsCheckStaticCallError{
      .code = NumberMethodsCheckStaticCallErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

NumberMethods::ResultStaticCallMade NumberMethods::checkStringRaw(CallExpr *call) {
  if (!call->tagged) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextNumberStatic::rawTag();

    return err(NumberMethodsCheckStaticCallError{
        .code = NumberMethodsCheckStaticCallErrorCode::RAW_NOT_TAGGED,
        .message = message,
    });
  }
  auto *texts = cast<ArrayLitExpr>(call->args[0]);
  auto *templateExpr = this->state.getAst().make<TemplateExpr>(texts->loc());
  for (const ArrayLitExpr::Element &element : texts->elements) {
    auto *text = cast<StringLitExpr>(element.expr);
    templateExpr->quasis.push_back(text->value);
  }
  templateExpr->exprs.assign(call->args.begin() + 1, call->args.end());
  auto *raw = this->literalChecker->checkTemplate(templateExpr);

  return ok(raw);
}

NumberMethods::ResultStaticCallMade NumberMethods::checkFromCodes(CallExpr *call, Builtin builtin) {
  for (Expr *&arg : call->args) {
    arg = this->checkCodeArg(arg, builtin);
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::string();

  return ok(call);
}

Expr *NumberMethods::checkCodeArg(Expr *arg, Builtin builtin) {
  auto *numeric = this->mathChecker->numericArg(arg);
  if (builtin == Builtin::FromCharCode) {
    return this->binaryChecker->toInt32(numeric);
  }
  if (numeric->type->isNumeric()) {
    this->walkCodePoint(numeric);
  }

  return this->typeRelations->convert(numeric, Type::doubleTy());
}

void NumberMethods::walkCodePoint(const Expr *arg) {
  auto what = ErrorTextNumberMethod::codePoint();
  auto between = this->indexProofs->requireBetween(arg, 0, kMaxCodePoint, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(arg->loc(), between.error().message);
  }
}

NumberMethods::ResultStaticCallMade NumberMethods::checkNumberParse(CallExpr *call,
                                                                    MemberExpr *member) {
  auto parsed = this->globalFunctionChecker->checkGlobalFunction(call, member->name, member->loc());
  bool isReported =
      parsed.isErr() &&
      parsed.error().code == GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED;
  if (isReported) {
    return err(NumberMethodsCheckStaticCallError{
        .code = NumberMethodsCheckStaticCallErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (parsed.isErr()) {
    return err(NumberMethodsCheckStaticCallError{
        .code = NumberMethodsCheckStaticCallErrorCode::PARSE_FAILED,
        .message = parsed.error().message,
    });
  }

  return ok(parsed.value());
}

NumberMethods::ResultStaticCallMade NumberMethods::checkNumberPredicate(CallExpr *call,
                                                                        Builtin builtin) {
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(NumberMethodsCheckStaticCallError{
        .code = NumberMethodsCheckStaticCallErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  call->args[0] = this->mathChecker->numericArg(call->args[0]);
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::boolTy();

  return ok(call);
}

NumberMethods::ResultConversionMade NumberMethods::checkConversion(CallExpr *call,
                                                                   MemberExpr *member) {
  const auto *to = this->conversionTarget(member->name);
  if (to == nullptr) {
    return err(NumberMethodsCheckConversionError{
        .code = NumberMethodsCheckConversionErrorCode::NOT_A_CONVERSION,
        .message = "",
    });
  }
  call->type = Type::error();
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(NumberMethodsCheckConversionError{
        .code = NumberMethodsCheckConversionErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  auto allowed = this->checkConversionAllowed(member, to);
  if (allowed.isErr()) {
    return err(allowed.error());
  }

  call->target = BuiltinCall{.builtin = Builtin::NumConvert};
  call->type = to;

  return ok(call);
}

const Type *NumberMethods::conversionTarget(llvm::StringRef name) const {
  const auto *method = NumberMethodTable::find(name);
  if (method == nullptr || method->convertsTo.empty()) {
    return nullptr;
  }

  return Type::fromName(method->convertsTo);
}

std::string NumberMethods::shownNumber(const Expr *object) const {
  auto *name = dyn_cast<NameExpr>(object);
  if (name == nullptr) {
    return kUnnamedNumber;
  }

  return name->name.str();
}

NumberMethods::ResultConversionMade NumberMethods::checkConversionAllowed(const MemberExpr *member,
                                                                          const Type *to) const {
  const Type *from = member->object->type;
  auto text = this->shownNumber(member->object);
  if (from->isFloat() && to->isDecimal()) {
    auto message = ErrorTextConversion::toDecimal(text);

    return err(NumberMethodsCheckConversionError{
        .code = NumberMethodsCheckConversionErrorCode::DOUBLE_TO_DECIMAL,
        .message = message,
    });
  }
  auto *rounding = dyn_cast<CallExpr>(member->object);
  bool isRounded = rounding != nullptr && BuiltinGroups::isRoundingBuiltin(rounding->builtin());
  if (from->isFloat() && to->isInteger() && !isRounded) {
    auto message = ErrorTextConversion::toInteger(text, member->name);

    return err(NumberMethodsCheckConversionError{
        .code = NumberMethodsCheckConversionErrorCode::DOUBLE_TO_INTEGER,
        .message = message,
    });
  }

  return ok(member->object);
}

NumberMethods::ResultMethodCallMade NumberMethods::checkNumberMethod(CallExpr *call,
                                                                     MemberExpr *member) {
  auto converted = this->checkConversion(call, member);
  if (converted.isOk()) {
    return ok(converted.value());
  }
  if (converted.error().code != NumberMethodsCheckConversionErrorCode::NOT_A_CONVERSION) {
    return err(NumberMethodsCheckNumberMethodError{
        .code = NumberMethodsCheckNumberMethodErrorCode::CONVERSION_FAILED,
        .message = converted.error().message,
    });
  }
  const auto *method = NumberMethodTable::find(member->name);
  bool isToString = method != nullptr && method->method == NumberMethod::TO_STRING;
  bool isToFixed = method != nullptr && method->method == NumberMethod::TO_FIXED;
  if (isToString && call->args.size() == 1) {
    auto *radix = this->checkRadixString(call, member);

    return ok(radix);
  }
  if (isToString) {
    return this->checkPlainString(call, member);
  }
  if (isToFixed) {
    return this->checkToFixed(call, member);
  }
  this->argumentChecker->walkArgsLoosely(call);
  auto message = ErrorTextNumberMethod::noProperty(member->name, member->object->type->name());

  return err(NumberMethodsCheckNumberMethodError{
      .code = NumberMethodsCheckNumberMethodErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

Expr *NumberMethods::checkRadixString(CallExpr *call, MemberExpr *member) {
  member->object = this->typeRelations->convert(member->object, Type::doubleTy());
  auto what = ErrorTextNumberMethod::radix();
  this->walkDigitsArg(call, kMinRadix, kMaxRadix, what);
  call->target = BuiltinCall{.builtin = Builtin::NumToRadix};
  call->type = Type::string();

  return call;
}

NumberMethods::ResultMethodCallMade NumberMethods::checkPlainString(CallExpr *call,
                                                                    MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(NumberMethodsCheckNumberMethodError{
        .code = NumberMethodsCheckNumberMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  auto *text = this->concatChecker->toStringOperand(member->object);
  call->args.push_back(text);
  call->target = BuiltinCall{.builtin = Builtin::StringOf};
  call->type = Type::string();

  return ok(call);
}

NumberMethods::ResultMethodCallMade NumberMethods::checkToFixed(CallExpr *call,
                                                                MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(NumberMethodsCheckNumberMethodError{
        .code = NumberMethodsCheckNumberMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  member->object = this->typeRelations->convert(member->object, Type::doubleTy());
  if (!call->args.empty()) {
    auto what = ErrorTextNumberMethod::digits();
    this->walkDigitsArg(call, 0, kMaxFixedDigits, what);
  }
  call->target = BuiltinCall{.builtin = Builtin::NumToFixed};
  call->type = Type::string();

  return ok(call);
}

void NumberMethods::walkDigitsArg(CallExpr *call, int64_t low, int64_t high,
                                  const std::string &what) {
  call->args[0] = this->memberChecker->checkPosition(call->args[0]);
  auto between = this->indexProofs->requireBetween(call->args[0], low, high, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
  }
}

}
