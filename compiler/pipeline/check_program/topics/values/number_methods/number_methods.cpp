#include "compiler/pipeline/check_program/topics/values/number_methods/number_methods.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/binary_checker/binary_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/concat_checker/concat_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/literal_checker/literal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker.h"
#include "compiler/pipeline/check_program/topics/math/math_checker/math_checker.h"
#include "compiler/pipeline/check_program/topics/values/global_function_checker/global_function_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

NumberMethods::NumberMethods(CheckState &owner)
    : state(owner) {}

NumberMethods::ResultStaticCallMade NumberMethods::checkStaticCall(CallExpr *call,
                                                                   MemberExpr *member) {
  bool isString = CallChecker(this->state).isGlobalName(member->object, "String");
  llvm::StringRef fn = member->name;
  call->type = Type::error();
  if (isString && fn == "raw" && !call->tagged) {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    std::string message = ErrorTextNumberStatic::rawTag();

    return err(NumberMethodsCheckStaticCallError{
        .code = NumberMethodsCheckStaticCallErrorCode::RAW_NOT_TAGGED,
        .message = message,
    });
  }
  if (isString && fn == "raw") {
    auto *texts = cast<ArrayLitExpr>(call->args[0]);
    auto *tmpl = this->state.getAst().make<TemplateExpr>(texts->loc());
    for (const ArrayLitExpr::Element &e : texts->elements) {
      tmpl->quasis.push_back(cast<StringLitExpr>(e.expr)->value);
    }
    tmpl->exprs.assign(call->args.begin() + 1, call->args.end());
    Expr *raw = LiteralChecker(this->state).checkTemplate(tmpl);
    return ok(raw);
  }
  if (isString && (fn == "fromCharCode" || fn == "fromCodePoint")) {
    bool codeUnits = fn == "fromCharCode";
    for (Expr *&arg : call->args) {
      arg = MathChecker(this->state).numericArg(arg);
      if (!codeUnits && arg->type->isNumeric()) {
        std::string what = ErrorTextNumberMethod::codePoint();
        IndexProofs::ResultBetweenProven between =
            IndexProofs(this->state).requireBetween(arg, 0, 0x10FFFF, what);
        if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
          this->state.getDiagnostics().error(arg->loc(), between.error().message);
        }
      }
      arg = codeUnits ? BinaryChecker(this->state).toInt32(arg)
                      : TypeRelations(this->state).convert(arg, Type::doubleTy());
    }
    call->target =
        BuiltinCall{.builtin = codeUnits ? Builtin::FromCharCode : Builtin::FromCodePoint};
    call->type = Type::string();
    return ok(call);
  }
  if (!isString && (fn == "parseInt" || fn == "parseFloat")) {
    GlobalFunctionChecker::ResultFunctionCalled parsed =
        GlobalFunctionChecker(this->state).checkGlobalFunction(call, fn, member->loc());
    bool reported =
        parsed.isErr() &&
        parsed.error().code == GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED;
    if (reported) {
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
  static const std::pair<llvm::StringLiteral, Builtin> predicates[] = {
      {"isInteger", Builtin::IsInteger},
      {"isSafeInteger", Builtin::IsSafeInteger},
  };
  if (!isString) {
    for (const auto &[name, builtin] : predicates) {
      if (fn != name) {
        continue;
      }
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 1);
      if (arity.isErr()) {
        return err(NumberMethodsCheckStaticCallError{
            .code = NumberMethodsCheckStaticCallErrorCode::WRONG_ARITY,
            .message = arity.error().message,
        });
      }
      call->args[0] = MathChecker(this->state).numericArg(call->args[0]);
      call->target = BuiltinCall{.builtin = builtin};
      call->type = Type::boolTy();
      return ok(call);
    }
  }

  ArgumentChecker(this->state).checkArgsLoosely(call);
  std::string message = ErrorTextNumberStatic::noProperty(fn, isString ? "String" : "Number");

  return err(NumberMethodsCheckStaticCallError{
      .code = NumberMethodsCheckStaticCallErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
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
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
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
  bool rounded = rounding && isRoundingBuiltin(rounding->builtin());
  if (from->isFloat() && to->isInteger() && !rounded) {
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
  bool conversion = converted.isOk() || converted.error().code !=
                                            NumberMethodsCheckConversionErrorCode::NOT_A_CONVERSION;
  if (converted.isErr() && conversion) {
    return err(NumberMethodsCheckNumberMethodError{
        .code = NumberMethodsCheckNumberMethodErrorCode::CONVERSION_FAILED,
        .message = converted.error().message,
    });
  }
  if (conversion) {
    return ok(converted.value());
  }
  if (member->name == "toString" && call->args.size() == 1) {
    member->object = TypeRelations(this->state).convert(member->object, Type::doubleTy());
    call->args[0] = MemberChecker(this->state).checkPosition(call->args[0]);
    std::string what = ErrorTextNumberMethod::radix();
    IndexProofs::ResultBetweenProven between =
        IndexProofs(this->state).requireBetween(call->args[0], 2, 36, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
    call->target = BuiltinCall{.builtin = Builtin::NumToRadix};
    call->type = Type::string();
    return ok(call);
  }
  if (member->name == "toString") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      return err(NumberMethodsCheckNumberMethodError{
          .code = NumberMethodsCheckNumberMethodErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    call->args.push_back(ConcatChecker(this->state).toStringOperand(member->object));
    call->target = BuiltinCall{.builtin = Builtin::StringOf};
    call->type = Type::string();
    return ok(call);
  }
  if (member->name == "toFixed") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      return err(NumberMethodsCheckNumberMethodError{
          .code = NumberMethodsCheckNumberMethodErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    member->object = TypeRelations(this->state).convert(member->object, Type::doubleTy());
    if (!call->args.empty()) {
      call->args[0] = MemberChecker(this->state).checkPosition(call->args[0]);
      std::string what = ErrorTextNumberMethod::digits();
      IndexProofs::ResultBetweenProven between =
          IndexProofs(this->state).requireBetween(call->args[0], 0, 100, what);
      if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
        this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
      }
    }
    call->target = BuiltinCall{.builtin = Builtin::NumToFixed};
    call->type = Type::string();
    return ok(call);
  }

  ArgumentChecker(this->state).checkArgsLoosely(call);
  std::string message =
      ErrorTextNumberMethod::noProperty(member->name, member->object->type->name());

  return err(NumberMethodsCheckNumberMethodError{
      .code = NumberMethodsCheckNumberMethodErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

}
