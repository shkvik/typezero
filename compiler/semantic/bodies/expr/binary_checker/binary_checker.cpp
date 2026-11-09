#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/bodies/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/expr/unary_checker/unary_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

BinaryChecker::BinaryChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dateChecker = &all.getDateChecker();
  this->exprChecker = &all.getExprChecker();
  this->jsonChecker = &all.getJsonChecker();
  this->resultTypes = &all.getResultTypes();
  this->matchChecker = &all.getMatchChecker();
  this->truthChecker = &all.getTruthChecker();
  this->unaryChecker = &all.getUnaryChecker();
  this->concatChecker = &all.getConcatChecker();
  this->typeRelations = &all.getTypeRelations();
  this->constantFolder = &all.getConstantFolder();
  this->decimalChecker = &all.getDecimalChecker();
  this->genericChecker = &all.getGenericChecker();
  this->instanceOfChecker = &all.getInstanceOfChecker();
}

bool BinaryChecker::onlyIntLiterals(const Expr *expr) {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!BinaryChecker::isIntLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool BinaryChecker::isIntLiteralNode(const Expr *expr) {
  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return bin->op == BinaryOp::Add || bin->op == BinaryOp::Sub || bin->op == BinaryOp::Mul ||
           bin->op == BinaryOp::Rem || bin->op == BinaryOp::Pow;
  }

  return false;
}

const Type *BinaryChecker::integerHint(const Expr *literal, const Expr *other, const Type *hint,
                                       bool bitwise) {
  const Type *type = other->type ? other->type->nonOptional() : nullptr;
  if (type && (type->isInteger() || (type->isFloat() && !bitwise)) &&
      BinaryChecker::onlyIntLiterals(literal) && !TypeRelations::isIntLiteralTree(other)) {
    return type;
  }

  return hint;
}

std::string BinaryChecker::operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs) {
  bool undefined =
      lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined();

  return ErrorTextOperands::types(op, lhs->name(), rhs->name(), undefined);
}

BinaryChecker::ResultExponentIsValid BinaryChecker::powerExponent(const Expr *rhs,
                                                                  const Type *common) const {
  std::optional<int64_t> exponent = this->constantFolder->intConstant(rhs);
  if (exponent && *exponent < 0) {
    std::string message = ErrorTextPower::negative(common->name());

    return err(BinaryCheckerPowerExponentError{
        .code = BinaryCheckerPowerExponentErrorCode::NEGATIVE_POWER,
        .message = message,
    });
  }

  return ok(rhs);
}

Expr *BinaryChecker::checkBinary(BinaryExpr *bin, const Type *expected, bool wraps) {
  const char *op = Operators::opSpelling(bin->op);
  if (expected) {
    expected = expected->nonOptional();
  }
  switch (bin->op) {
    case BinaryOp::In: {
      UnaryChecker::ResultInOperatorSuited in = this->unaryChecker->checkIn(bin);
      if (in.isOk()) {
        return in.value();
      }
      llvm::SMLoc at = bin->loc();
      switch (in.error().code) {
        case UnaryCheckerCheckInErrorCode::DICT_UNDEFINED:
        case UnaryCheckerCheckInErrorCode::UNION_UNDEFINED:
          at = bin->rhs->loc();
          break;
        case UnaryCheckerCheckInErrorCode::DICT_NUMBER_KEY:
          at = bin->lhs->loc();
          break;
        case UnaryCheckerCheckInErrorCode::DICT_NOT_RECORD:
        case UnaryCheckerCheckInErrorCode::UNION_NO_KEY:
        case UnaryCheckerCheckInErrorCode::OPTIONAL_FIELD:
        case UnaryCheckerCheckInErrorCode::ALWAYS_SAME:
        case UnaryCheckerCheckInErrorCode::TUPLE_IN:
        case UnaryCheckerCheckInErrorCode::OBJECT_OPTIONAL:
        case UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_TRUE:
        case UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_FALSE:
        case UnaryCheckerCheckInErrorCode::OBJECT_IN:
        case UnaryCheckerCheckInErrorCode::OPERAND_TYPES:
        case UnaryCheckerCheckInErrorCode::ARRAY_IN:
          break;
      }
      this->state.getDiagnostics().error(at, in.error().message);
      return bin;
    }

    case BinaryOp::InstanceOf: {
      InstanceOfChecker::ResultInstanceTested tested =
          this->instanceOfChecker->checkInstanceOf(bin);
      if (tested.isOk()) {
        return tested.value();
      }
      llvm::SMLoc at = bin->loc();
      switch (tested.error().code) {
        case InstanceOfCheckerCheckInstanceOfErrorCode::NOT_AN_OBJECT:
          at = bin->lhs->loc();
          break;
        case InstanceOfCheckerCheckInstanceOfErrorCode::NOT_A_CLASS:
          at = bin->rhs->loc();
          break;
        case InstanceOfCheckerCheckInstanceOfErrorCode::UNTYPED_COLLECTION:
        case InstanceOfCheckerCheckInstanceOfErrorCode::NONE_MATCHES:
        case InstanceOfCheckerCheckInstanceOfErrorCode::ALL_MATCH:
        case InstanceOfCheckerCheckInstanceOfErrorCode::ALWAYS_TRUE:
        case InstanceOfCheckerCheckInstanceOfErrorCode::TEST_UNDEFINED:
        case InstanceOfCheckerCheckInstanceOfErrorCode::ALWAYS_FALSE:
        case InstanceOfCheckerCheckInstanceOfErrorCode::BYTES_INSTANCE:
        case InstanceOfCheckerCheckInstanceOfErrorCode::GENERIC_UNKNOWN:
        case InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_GENERIC:
        case InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_IN_UNION:
        case InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_CLASS:
        case InstanceOfCheckerCheckInstanceOfErrorCode::NOT_IMPLEMENTED:
        case InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_CREATED:
          break;
      }
      this->state.getDiagnostics().error(at, tested.error().message);
      return bin;
    }

  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    Flow whenTrue, whenFalse;
    this->truthChecker->checkLogical(bin, expected, whenTrue, whenFalse, false,
                                     bin->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or);
    this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
    return bin;
  }

  case BinaryOp::Nullish: {
    ResultNullishResolved nullish = this->checkNullish(bin, expected);
    if (nullish.isErr()) {
      this->state.getDiagnostics().error(bin->loc(), nullish.error().message);
      return bin;
    }
    return nullish.value();
  }

  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return this->checkEquality(bin);

  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    this->decimalChecker->checkComparisonOperands(bin);
    this->resultTypes->neverOperands(bin);
    bin->type = Type::boolTy();
    if (bin->lhs->type->isParam() && bin->lhs->type == bin->rhs->type) {
      return bin;
    }
    bin->lhs = this->genericChecker->apparent(bin->lhs);
    bin->rhs = this->genericChecker->apparent(bin->rhs);
    const Type *l = bin->lhs->type;
    const Type *r = bin->rhs->type;
    if (l->isError() || r->isError()) {
      return bin;
    }
    if (l->isString() && r->isString()) {
      return bin;
    }
    if (Expr *dates = this->dateChecker->checkDateOperands(bin)) {
      return dates;
    }
    if (l->isDecimal() || r->isDecimal()) {
      return this->decimalChecker->checkDecimalComparison(bin);
    }
    if (l->isNumeric() && r->isNumeric()) {
      this->adaptLiterals(bin);
      l = bin->lhs->type;
      r = bin->rhs->type;
      if (l == r || (l->isInteger() && r->isInteger())) {
        return bin;
      }
      std::string message = BinaryChecker::mixedNumbersText(bin);
      this->state.getDiagnostics().error(bin->loc(), message);
      return bin;
    }
    std::string message = BinaryChecker::operandTypesText(op, l, r);
    this->state.getDiagnostics().error(bin->loc(), message);
    return bin;
  }

  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    const Type *hint =
        expected && expected->nonOptional()->isInteger() ? expected->nonOptional() : nullptr;
    bin->lhs = this->genericChecker->apparent(this->exprChecker->checkExpr(
        bin->lhs, BinaryChecker::onlyIntLiterals(bin->lhs) ? hint : nullptr));
    bin->rhs = this->genericChecker->apparent(this->exprChecker->checkExpr(bin->rhs));
    ResultBitwiseResolved bits = this->checkBits(bin);
    if (bits.isErr()) {
      bool divisor = bits.error().code == BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO;
      llvm::SMLoc at = divisor ? bin->rhs->loc() : bin->loc();
      this->state.getDiagnostics().error(at, bits.error().message);
      return bin;
    }
    return bits.value();
  }

  default: {
    bool bitwise =
        bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr || bin->op == BinaryOp::BitXor;
    const Type *hint = nullptr;
    if (expected && !bitwise && (expected->isNumeric() || expected->isDecimal())) {
      hint = expected;
    }
    bool pow = bin->op == BinaryOp::Pow;
    const Type *rhsHint = pow && hint && hint->isDecimal() ? nullptr : hint;
    bool operandsWrap = false;
    ExpressionMarks &marks = this->state.getExpressionMarks();
    BinaryExpr *chain = marks.takeConcatChain();
    bool chainEnd = bin->op == BinaryOp::Add && !chain;
    const BinaryExpr *outerMixed = chainEnd ? marks.takeMixedConcat() : nullptr;
    if (auto *left = dyn_cast<BinaryExpr>(bin->lhs);
        bin->op == BinaryOp::Add && left && left->op == BinaryOp::Add) {
      marks.rememberConcatChain(chain ? chain : bin);
    }
    bool literals = !bitwise && !pow;
    if (DecimalChecker::isLiteralTree(bin->lhs) && !DecimalChecker::isLiteralTree(bin->rhs) &&
        (literals || BinaryChecker::onlyIntLiterals(bin->lhs))) {
      BinaryExpr *pending = marks.takeConcatChain();
      bin->rhs = this->genericChecker->apparent(
          this->exprChecker->checkExpr(bin->rhs, rhsHint, operandsWrap));
      marks.rememberConcatChain(pending);
      const Type *lhsHint = BinaryChecker::integerHint(
          bin->lhs, bin->rhs, literals ? DecimalChecker::literalHint(bin->rhs, hint) : hint,
          bitwise);
      bin->lhs = this->genericChecker->apparent(
          this->exprChecker->checkExpr(bin->lhs, lhsHint, operandsWrap));
      marks.forgetConcatChain();
    } else {
      bin->lhs = this->genericChecker->apparent(
          this->exprChecker->checkExpr(bin->lhs, hint, operandsWrap));
      marks.forgetConcatChain();
      if (literals && DecimalChecker::isLiteralTree(bin->rhs)) {
        rhsHint = DecimalChecker::literalHint(bin->lhs, rhsHint);
      }
      rhsHint = BinaryChecker::integerHint(bin->rhs, bin->lhs, rhsHint, bitwise);
      bin->rhs = this->genericChecker->apparent(
          this->exprChecker->checkExpr(bin->rhs, rhsHint, operandsWrap));
    }
    this->resultTypes->neverOperands(bin);
    this->adaptLiterals(bin);
    const Type *l = bin->lhs->type;
    const Type *r = bin->rhs->type;
    if (bin->op == BinaryOp::Add) {
      auto joinsAsText = [](const Type *type) {
        const Type *base = type->nonOptional();

        return base->isNumeric() || base->isDecimal() || base->isBool();
      };
      if (!marks.getMixedConcat() &&
          ((l->isString() && joinsAsText(r)) || (r->isString() && joinsAsText(l)))) {
        marks.rememberMixedConcat(bin);
      }
      if (chainEnd) {
        if (const BinaryExpr *first = marks.getMixedConcat()) {
          std::string mixed = ConcatChecker::mixedConcatText(bin, first);
          this->state.getDiagnostics().error(first->loc(), mixed);
        }
        marks.rememberMixedConcat(outerMixed);
      }
    }
    bin->type = Type::error();
    if (l->isError() || r->isError()) {
      return bin;
    }
    if (bin->op == BinaryOp::Add && (l->isString() || r->isString())) {
      bin->lhs = this->concatChecker->toStringOperand(bin->lhs);
      bin->rhs = this->concatChecker->toStringOperand(bin->rhs);
      bin->type = Type::string();
      return bin;
    }
    if (Expr *dates = this->dateChecker->checkDateOperands(bin)) {
      return dates;
    }
    if (bitwise) {
      ResultBitwiseResolved bits = this->checkBits(bin);
      if (bits.isErr()) {
        bool divisor = bits.error().code == BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO;
        llvm::SMLoc at = divisor ? bin->rhs->loc() : bin->loc();
        this->state.getDiagnostics().error(at, bits.error().message);
        return bin;
      }
      return bits.value();
    }
    if (l->isDecimal() || r->isDecimal()) {
      return this->decimalChecker->checkDecimalArithmetic(bin);
    }
    if ((l->isWrapping() || r->isWrapping()) && l->isInteger() && r->isInteger()) {
      ResultWrappingChecked wrapped = this->checkWrapping(bin);
      if (wrapped.isErr()) {
        bool divisor = wrapped.error().code == BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO;
        llvm::SMLoc at = divisor ? bin->rhs->loc() : bin->loc();
        this->state.getDiagnostics().error(at, wrapped.error().message);
        return bin;
      }
      return wrapped.value();
    }
    if (bin->op == BinaryOp::Pow && l->isFloat() && r->isInteger()) {
      bin->rhs = this->typeRelations->convert(bin->rhs, l);
      r = l;
    }
    bool operandsOk = bitwise ? l->isInteger() && r->isInteger() : l->isNumeric() && r->isNumeric();
    const Type *common = operandsOk ? TypeRelations::commonNumericType(l, r) : nullptr;
    if (!common && operandsOk) {
      std::string message = BinaryChecker::mixedNumbersText(bin);
      this->state.getDiagnostics().error(bin->loc(), message);
      return bin;
    }
    if (!common) {
      std::string message = BinaryChecker::operandTypesText(op, l, r);
      this->state.getDiagnostics().error(bin->loc(), message);
      return bin;
    }
    bin->lhs = this->typeRelations->convert(bin->lhs, common);
    bin->rhs = this->typeRelations->convert(bin->rhs, common);
    bin->type = common;
    if (!common->isInteger()) {
      return bin;
    }
    if (bin->op == BinaryOp::Pow) {
      ResultExponentIsValid power = this->powerExponent(bin->rhs, common);
      if (power.isErr()) {
        this->state.getDiagnostics().error(bin->rhs->loc(), power.error().message);
        return bin;
      }
    }
    ConstantFolder::ResultDivisorNotZero divisor =
        this->constantFolder->checkDivisor(bin->op, bin->rhs);
    if (divisor.isErr()) {
      this->state.getDiagnostics().error(bin->rhs->loc(), divisor.error().message);
    }
    this->constantFolder->foldBinary(bin, wraps);
    return bin;
  }
  }
}

BinaryChecker::ResultWrappingChecked BinaryChecker::checkWrapping(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isWrapping() && r->isWrapping() && l != r) {
    std::string message = BinaryChecker::mixedNumbersText(bin);

    return err(BinaryCheckerCheckWrappingError{
        .code = BinaryCheckerCheckWrappingErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }
  const Type *wrap = l->isWrapping() ? l : r;
  bin->lhs = this->typeRelations->convert(bin->lhs, wrap);
  bin->rhs = this->typeRelations->convert(bin->rhs, wrap);
  bin->type = wrap;
  ConstantFolder::ResultDivisorNotZero divisor =
      this->constantFolder->checkDivisor(bin->op, bin->rhs);
  if (divisor.isErr()) {
    return err(BinaryCheckerCheckWrappingError{
        .code = BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO,
        .message = divisor.error().message,
    });
  }

  return ok(bin);
}

BinaryChecker::ResultBitwiseResolved BinaryChecker::checkBits(BinaryExpr *bin) {
  const char *op = Operators::opSpelling(bin->op);
  bool shift = bin->op == BinaryOp::Shl || bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  this->adaptLiterals(bin);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    return ok(bin);
  }
  auto zero = [](const Expr *e) {
    auto *lit = dyn_cast<IntLitExpr>(e);

    return lit && lit->value == 0;
  };
  auto [lhsText, rhsText] = DecimalChecker::sidesOf(bin);
  std::string text = lhsText.empty() ? "x" : TypeRelations::operandText(lhsText);
  bool orZero = l->isNumeric() && !l->isWrapping() && zero(bin->rhs) &&
                (bin->op == BinaryOp::BitOr || bin->op == BinaryOp::UShr);
  if (orZero) {
    std::string lhsName = lhsText.empty() ? "x" : lhsText.str();
    auto *rounded = dyn_cast<CallExpr>(bin->lhs);
    bool roundedFloat =
        l->isFloat() && rounded && BuiltinGroups::isRoundingBuiltin(rounded->builtin());
    std::string hint = roundedFloat   ? ErrorTextNoOpBits::toInt(lhsText)
                       : l->isFloat() ? ErrorTextNoOpBits::trunc(lhsText)
                                      : ErrorTextNoOpBits::wrap(text, bin->op == BinaryOp::UShr);
    std::string message = ErrorTextNoOpBits::nothing(lhsName, op, hint);

    return err(BinaryCheckerCheckBitsError{
        .code = BinaryCheckerCheckBitsErrorCode::NOTHING_CHANGES,
        .message = message,
    });
  }
  if (l->isFloat() || r->isFloat()) {
    std::string side = l->isFloat()      ? text
                       : rhsText.empty() ? "x"
                                         : TypeRelations::operandText(rhsText);
    std::string message = ErrorTextBitsDouble::fraction(op, side);

    return err(BinaryCheckerCheckBitsError{
        .code = BinaryCheckerCheckBitsErrorCode::DOUBLE_OPERAND,
        .message = message,
    });
  }
  if (!l->isInteger() || !r->isInteger()) {
    std::string message = BinaryChecker::operandTypesText(op, l, r);

    return err(BinaryCheckerCheckBitsError{
        .code = BinaryCheckerCheckBitsErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool signedShift = shift && bin->op == BinaryOp::UShr && !l->isUnsigned();
  if (signedShift) {
    std::string count = rhsText.empty() ? "n" : rhsText.str();
    const Type *unsignedType = Type::integer(l->bitWidth(), true);
    std::string method = TypeRelations::conversionMethod(unsignedType);
    std::string message = ErrorTextShift::isSigned(l->name(), text, method, count);

    return err(BinaryCheckerCheckBitsError{
        .code = BinaryCheckerCheckBitsErrorCode::SIGNED_SHIFT,
        .message = message,
    });
  }
  if (shift) {
    bin->rhs = this->typeRelations->convert(bin->rhs, l);
    bin->type = l;
    this->constantFolder->foldBinary(bin, false);
    return ok(bin);
  }
  if (l->isWrapping() || r->isWrapping()) {
    ResultWrappingChecked wrapped = this->checkWrapping(bin);
    if (wrapped.isOk()) {
      return ok(wrapped.value());
    }
    bool divisor = wrapped.error().code == BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO;

    return err(BinaryCheckerCheckBitsError{
        .code = divisor ? BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO
                        : BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS,
        .message = wrapped.error().message,
    });
  }
  if (l != r) {
    std::string message = BinaryChecker::mixedNumbersText(bin);

    return err(BinaryCheckerCheckBitsError{
        .code = BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }
  bin->type = l;
  this->constantFolder->foldBinary(bin, false);

  return ok(bin);
}

void BinaryChecker::adaptLiterals(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (!l || !r || l == r || l->isError() || r->isError()) {
    return;
  }
  auto number = [](const Type *t) { return t->isNumeric() || t->isDecimal(); };
  bool bitwise = bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr ||
                 bin->op == BinaryOp::BitXor || bin->op == BinaryOp::Shl ||
                 bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  if (!number(l) || !number(r) || (bitwise && (!l->isInteger() || !r->isInteger()))) {
    return;
  }
  if (this->typeRelations->literalAdapts(bin->lhs, r) &&
      !(bin->op == BinaryOp::Pow && r->isDecimal())) {
    bin->lhs = this->typeRelations->adaptLiteral(bin->lhs, r);
  } else if (this->typeRelations->literalAdapts(bin->rhs, l) &&
             !(bin->op == BinaryOp::Pow && (l->isDecimal() || l->isFloat()))) {
    bin->rhs = this->typeRelations->adaptLiteral(bin->rhs, l);
  }
}

std::string BinaryChecker::mixedNumbersText(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  const char *op = Operators::opSpelling(bin->op);
  const Type *target = nullptr;
  bool convertLeft = false;
  if (l->isFloat() || r->isFloat()) {
    target = Type::doubleTy();
    convertLeft = !l->isFloat();
  } else if (l->holds(r)) {
    target = l;
  } else if (r->holds(l)) {
    target = r;
    convertLeft = true;
  } else {
    target = Type::intTy();
    convertLeft = !l->isInt();
  }
  auto [lhs, rhs] = DecimalChecker::sidesOf(bin);
  std::string method = TypeRelations::conversionMethod(target);
  std::string example;
  if (!lhs.empty() && !rhs.empty()) {
    std::string left =
        convertLeft ? TypeRelations::operandText(lhs) + "." + method + "()" : lhs.str();
    std::string right =
        convertLeft ? rhs.str() : TypeRelations::operandText(rhs) + "." + method + "()";
    example = ErrorTextMixedNumbers::example(left, op, right);
  } else {
    example = ErrorTextMixedNumbers::method(method);
  }

  return ErrorTextMixedNumbers::mixed(op, l->name(), r->name(), example);
}

std::string BinaryChecker::quotedNames(llvm::ArrayRef<llvm::StringRef> names) {
  std::string list;
  for (size_t i = 0; i < names.size(); ++i) {
    list += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + ("\"" + names[i].str() + "\"");
  }

  return list;
}

BinaryChecker::ResultTypeOfNameKnown BinaryChecker::checkTypeOfName(const Expr *operand,
                                                                    const StringLitExpr *text) {
  std::string name = ToUtf8::toUtf8(text->value);
  if (name == "bigint") {
    std::string what = ErrorTextTypeOfName::bigint();
    std::string bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(text->loc(), bigint);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!TypeSpelling::isTypeOfName(name)) {
    std::vector<llvm::StringRef> all(std::begin(kTypeOfNames), std::end(kTypeOfNames));
    std::string list = BinaryChecker::quotedNames(all);
    if (name == "number") {
      std::vector<llvm::StringRef> numbers;
      if (operand->type && !operand->type->isError()) {
        for (llvm::StringRef possible : TypeSpelling::typeOfNames(operand->type)) {
          if (possible.starts_with("int") || possible.starts_with("uint") ||
              possible == "double" || possible == "decimal") {
            numbers.push_back(possible);
          }
        }
      }
      std::string here = numbers.empty() || numbers.size() > 3
                             ? std::string()
                             : ErrorTextTypeOfName::forType(operand->type->name(),
                                                            BinaryChecker::quotedNames(numbers));
      std::string message = ErrorTextTypeOfName::noNumber(here);

      return err(BinaryCheckerCheckTypeOfNameError{
          .code = BinaryCheckerCheckTypeOfNameErrorCode::NO_NUMBER,
          .message = message,
      });
    }
    if (name == "symbol") {
      std::string message = ErrorTextTypeOfName::noSymbol(list);

      return err(BinaryCheckerCheckTypeOfNameError{
          .code = BinaryCheckerCheckTypeOfNameErrorCode::NO_SYMBOL,
          .message = message,
      });
    }
    std::string message = ErrorTextTypeOfName::notName(name, list);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::NOT_A_NAME,
        .message = message,
    });
  }
  const Type *type = operand->type;
  bool unchecked = !type || type->isError() || this->state.getGenericStore().isCheckingInstance() ||
                   type->nonOptional()->isParam();
  if (unchecked) {
    return ok(text);
  }
  std::vector<llvm::StringRef> names = TypeSpelling::typeOfNames(type);
  bool matches = names.empty() || llvm::is_contained(names, llvm::StringRef(name));
  if (!matches) {
    std::string quoted = BinaryChecker::quotedNames(names);
    std::string message = ErrorTextTypeOfName::never(type->name(), quoted, name);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::NEVER_MATCHES,
        .message = message,
    });
  }

  return ok(text);
}

BinaryChecker::ResultNoResultOperand BinaryChecker::noResults(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->nonOptional()->isResult() || r->nonOptional()->isResult()) {
    std::string message = ErrorTextResultEquality::compared(Operators::opSpelling(bin->op));

    return err(BinaryCheckerNoResultsError{
        .code = BinaryCheckerNoResultsErrorCode::RESULTS_COMPARED,
        .message = message,
    });
  }

  return ok(bin);
}

Expr *BinaryChecker::checkEquality(BinaryExpr *bin) {
  if (Expr *null = this->jsonChecker->checkNullEquality(bin)) {
    return null;
  }
  this->decimalChecker->checkComparisonOperands(bin);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bin->type = Type::boolTy();
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (typeOf && typeOf->op == UnaryOp::TypeOf && text) {
      ResultTypeOfNameKnown known = this->checkTypeOfName(typeOf->operand, text);
      if (known.isErr() &&
          known.error().code != BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED) {
        this->state.getDiagnostics().error(text->loc(), known.error().message);
      }
    }
  }
  if (l->isError() || r->isError()) {
    return bin;
  }
  if (l->isVoid() || r->isVoid()) {
    std::string message = BinaryChecker::operandTypesText(Operators::opSpelling(bin->op), l, r);
    this->state.getDiagnostics().error(bin->loc(), message);
    return bin;
  }
  if (l->isUndefined() || r->isUndefined()) {
    return bin;
  }
  if (l->nonOptional()->isJson() || r->nonOptional()->isJson()) {
    return this->jsonChecker->checkJsonEquality(bin);
  }
  ResultNoResultOperand plain = BinaryChecker::noResults(bin);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(bin->loc(), plain.error().message);
    return bin;
  }
  if (l->nonOptional()->isParam() || r->nonOptional()->isParam()) {
    return bin;
  }

  this->adaptLiterals(bin);
  l = bin->lhs->type;
  r = bin->rhs->type;
  if (l->isInteger() && r->isInteger()) {
    return bin;
  }
  const Type *common = TypeRelations::comparisonType(l, r);
  if (!common) {
    if (l->nonOptional()->isDecimal() || r->nonOptional()->isDecimal()) {
      this->decimalChecker->reportDecimalMix(bin);
    } else if (l->nonOptional()->isNumeric() && r->nonOptional()->isNumeric() && !l->isOptional() &&
               !r->isOptional()) {
      std::string message = BinaryChecker::mixedNumbersText(bin);
      this->state.getDiagnostics().error(bin->loc(), message);
    } else {
      std::string message = BinaryChecker::operandTypesText(Operators::opSpelling(bin->op), l, r);
      this->state.getDiagnostics().error(bin->loc(), message);
    }
    return bin;
  }
  if (common->nonOptional()->isFunction()) {
    this->state.getInventoryStore().markComparesFunctions();
  }
  bin->lhs = this->typeRelations->convert(bin->lhs, common);
  bin->rhs = this->typeRelations->convert(bin->rhs, common);

  return bin;
}

BinaryChecker::ResultNullishResolved BinaryChecker::checkNullish(BinaryExpr *bin,
                                                                 const Type *expected) {
  bin->lhs = this->exprChecker->checkExpr(bin->lhs, expected);
  if (bin->lhs->type->nonOptional()->isMatch() && isa<ArrayLitExpr>(bin->rhs)) {
    bin->lhs = this->matchChecker->matchItems(bin->lhs);
  }
  const Type *l = bin->lhs->type;
  Flow afterLhs = this->state.getFlowState().getFlow();
  const Type *hint = l->isUndefined() || l->isError() ? expected : l->nonOptional();
  bin->rhs = this->exprChecker->checkExpr(bin->rhs, hint);
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(afterLhs, this->state.getFlowState().getFlow()));
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    if (this->state.getFunctionStack().isInsideTrial() && l->isError() != r->isError()) {
      bin->type = l->isError() ? r : l->nonOptional();
    }
    return ok(bin);
  }
  if (l->isVoid() || r->isVoid()) {
    std::string message = BinaryChecker::operandTypesText("??", l, r);

    return err(BinaryCheckerCheckNullishError{
        .code = BinaryCheckerCheckNullishErrorCode::VOID_OPERAND,
        .message = message,
    });
  }
  if (l->isUndefined()) {
    bin->type = r;
    return ok(bin);
  }
  const Type *base = r->isUndefined()
                         ? l->nonOptional()
                         : LiteralTypes::unionUnify(l->nonOptional(), r->nonOptional());
  if (!base || base->isError()) {
    std::string message = BinaryChecker::operandTypesText("??", l, r);

    return err(BinaryCheckerCheckNullishError{
        .code = BinaryCheckerCheckNullishErrorCode::NO_COMMON_TYPE,
        .message = message,
    });
  }
  const Type *result = r->isOptional() || r->isUndefined() ? Type::optional(base) : base;
  bin->lhs =
      this->typeRelations->convert(bin->lhs, l->isOptional() ? Type::optional(base) : result);
  bin->rhs = this->typeRelations->convert(bin->rhs, result);
  bin->type = result;

  return ok(bin);
}

Expr *BinaryChecker::toInt32(Expr *expr) {
  return expr->type->isInt() || expr->type->isError()
             ? expr
             : this->typeRelations->convert(expr, Type::intTy());
}

}
