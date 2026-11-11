#include "compiler/semantic/bodies/values/number_methods/number_methods.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
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
  std::optional<StringStatic> found = StringStaticTable::find(member->name);
  if (!found.has_value()) {
    return this->noSuchProperty(call, member->name, "String");
  }
  switch (*found) {
    case StringStatic::RAW:
      return this->checkStringRaw(call);
    case StringStatic::FROM_CHAR_CODE:
      return this->checkFromCodes(call, Builtin::FromCharCode);
    case StringStatic::FROM_CODE_POINT:
      return this->checkFromCodes(call, Builtin::FromCodePoint);
  }

  return this->noSuchProperty(call, member->name, "String");
}

NumberMethods::ResultStaticCallMade NumberMethods::checkNumberStatic(CallExpr *call,
                                                                     MemberExpr *member) {
  std::optional<NumberStatic> found = NumberStaticTable::find(member->name);
  if (!found.has_value()) {
    return this->noSuchProperty(call, member->name, "Number");
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

  return this->noSuchProperty(call, member->name, "Number");
}

NumberMethods::ResultStaticCallMade
NumberMethods::noSuchProperty(CallExpr *call, llvm::StringRef name, llvm::StringRef owner) {
  this->argumentChecker->walkArgsLoosely(call);
  std::string message = ErrorTextNumberStatic::noProperty(name, owner);

  return err(NumberMethodsCheckStaticCallError{
      .code = NumberMethodsCheckStaticCallErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

NumberMethods::ResultStaticCallMade NumberMethods::checkStringRaw(CallExpr *call) {
  if (!call->tagged) {
    this->argumentChecker->walkArgsLoosely(call);
    std::string message = ErrorTextNumberStatic::rawTag();

    return err(NumberMethodsCheckStaticCallError{
        .code = NumberMethodsCheckStaticCallErrorCode::RAW_NOT_TAGGED,
        .message = message,
    });
  }
  auto *texts = cast<ArrayLitExpr>(call->args[0]);
  auto *tmpl = this->state.getAst().make<TemplateExpr>(texts->loc());
  for (const ArrayLitExpr::Element &e : texts->elements) {
    tmpl->quasis.push_back(cast<StringLitExpr>(e.expr)->value);
  }
  tmpl->exprs.assign(call->args.begin() + 1, call->args.end());
  Expr *raw = this->literalChecker->checkTemplate(tmpl);

  return ok(raw);
}

NumberMethods::ResultStaticCallMade NumberMethods::checkFromCodes(CallExpr *call, Builtin builtin) {
  bool isCodeUnits = builtin == Builtin::FromCharCode;
  for (Expr *&arg : call->args) {
    arg = this->mathChecker->numericArg(arg);
    if (!isCodeUnits && arg->type->isNumeric()) {
      this->walkCodePoint(arg);
    }
    arg = isCodeUnits ? this->binaryChecker->toInt32(arg)
                      : this->typeRelations->convert(arg, Type::doubleTy());
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::string();

  return ok(call);
}

void NumberMethods::walkCodePoint(const Expr *arg) {
  std::string what = ErrorTextNumberMethod::codePoint();
  IndexProofs::ResultBetweenProven between =
      this->indexProofs->requireBetween(arg, 0, 0x10FFFF, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(arg->loc(), between.error().message);
  }
}

NumberMethods::ResultStaticCallMade NumberMethods::checkNumberParse(CallExpr *call,
                                                                    MemberExpr *member) {
  GlobalFunctionChecker::ResultFunctionCalled parsed =
      this->globalFunctionChecker->checkGlobalFunction(call, member->name, member->loc());
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
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
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
  static const std::pair<llvm::StringLiteral, llvm::StringLiteral> conversions[] = {
      {"toInt", "int"},         {"toInt8", "int8"},     {"toInt16", "int16"},
      {"toInt32", "int32"},     {"toUint8", "uint8"},   {"toUint16", "uint16"},
      {"toUint32", "uint32"},   {"toUint64", "uint64"}, {"toDouble", "double"},
      {"toDecimal", "decimal"},
  };
  const Type *to = nullptr;
  for (const auto &[method, type] : conversions) {
    if (member->name == method) {
      to = Type::fromName(type);
    }
  }
  if (!to) {
    return err(NumberMethodsCheckConversionError{
        .code = NumberMethodsCheckConversionErrorCode::NOT_A_CONVERSION,
        .message = "",
    });
  }
  call->type = Type::error();
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(NumberMethodsCheckConversionError{
        .code = NumberMethodsCheckConversionErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  const Type *from = member->object->type;
  std::string text = "x";
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    text = name->name.str();
  }
  if (from->isFloat() && to->isDecimal()) {
    std::string message = ErrorTextConversion::toDecimal(text);

    return err(NumberMethodsCheckConversionError{
        .code = NumberMethodsCheckConversionErrorCode::DOUBLE_TO_DECIMAL,
        .message = message,
    });
  }
  auto *rounding = dyn_cast<CallExpr>(member->object);
  bool isRounded = rounding && BuiltinGroups::isRoundingBuiltin(rounding->builtin());
  if (from->isFloat() && to->isInteger() && !isRounded) {
    std::string message = ErrorTextConversion::toInteger(text, member->name);

    return err(NumberMethodsCheckConversionError{
        .code = NumberMethodsCheckConversionErrorCode::DOUBLE_TO_INTEGER,
        .message = message,
    });
  }

  call->target = BuiltinCall{.builtin = Builtin::NumConvert};
  call->type = to;

  return ok(call);
}

NumberMethods::ResultMethodCallMade NumberMethods::checkNumberMethod(CallExpr *call,
                                                                     MemberExpr *member) {
  ResultConversionMade converted = this->checkConversion(call, member);
  bool isConversion =
      converted.isOk() ||
      converted.error().code != NumberMethodsCheckConversionErrorCode::NOT_A_CONVERSION;
  if (converted.isErr() && isConversion) {
    return err(NumberMethodsCheckNumberMethodError{
        .code = NumberMethodsCheckNumberMethodErrorCode::CONVERSION_FAILED,
        .message = converted.error().message,
    });
  }
  if (isConversion) {
    return ok(converted.value());
  }
  if (member->name == "toString" && call->args.size() == 1) {
    Expr *radix = this->checkRadixString(call, member);

    return ok(radix);
  }
  if (member->name == "toString") {
    return this->checkPlainString(call, member);
  }
  if (member->name == "toFixed") {
    return this->checkToFixed(call, member);
  }
  this->argumentChecker->walkArgsLoosely(call);
  std::string message =
      ErrorTextNumberMethod::noProperty(member->name, member->object->type->name());

  return err(NumberMethodsCheckNumberMethodError{
      .code = NumberMethodsCheckNumberMethodErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

Expr *NumberMethods::checkRadixString(CallExpr *call, MemberExpr *member) {
  member->object = this->typeRelations->convert(member->object, Type::doubleTy());
  std::string what = ErrorTextNumberMethod::radix();
  this->walkDigitsArg(call, 2, 36, what);
  call->target = BuiltinCall{.builtin = Builtin::NumToRadix};
  call->type = Type::string();

  return call;
}

NumberMethods::ResultMethodCallMade NumberMethods::checkPlainString(CallExpr *call,
                                                                    MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(NumberMethodsCheckNumberMethodError{
        .code = NumberMethodsCheckNumberMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  Expr *text = this->concatChecker->toStringOperand(member->object);
  call->args.push_back(text);
  call->target = BuiltinCall{.builtin = Builtin::StringOf};
  call->type = Type::string();

  return ok(call);
}

NumberMethods::ResultMethodCallMade NumberMethods::checkToFixed(CallExpr *call,
                                                                MemberExpr *member) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(NumberMethodsCheckNumberMethodError{
        .code = NumberMethodsCheckNumberMethodErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  member->object = this->typeRelations->convert(member->object, Type::doubleTy());
  if (!call->args.empty()) {
    std::string what = ErrorTextNumberMethod::digits();
    this->walkDigitsArg(call, 0, 100, what);
  }
  call->target = BuiltinCall{.builtin = Builtin::NumToFixed};
  call->type = Type::string();

  return ok(call);
}

void NumberMethods::walkDigitsArg(CallExpr *call, int64_t lo, int64_t hi, const std::string &what) {
  call->args[0] = this->memberChecker->checkPosition(call->args[0]);
  IndexProofs::ResultBetweenProven between =
      this->indexProofs->requireBetween(call->args[0], lo, hi, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
  }
}

}
