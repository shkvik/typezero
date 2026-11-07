#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/expr/assign_checker/assign_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/expr/new_checker/new_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/expr/unary_checker/unary_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/objects/object_literal_checker/object_literal_checker.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/values/array_literal_checker/array_literal_checker.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ExprChecker::ExprChecker(Checker &owner)
    : checker(owner) {}

Expr *ExprChecker::checkExpr(Expr *expr, const Type *expected, bool wraps) {
  if (this->checker.checkingInstance_ && !this->checker.forced_.empty()) {
    auto found = this->checker.forced_.find({expr->loc().getPointer(), unsigned(expr->kind())});
    if (found != this->checker.forced_.end()) {
      const Type *to = found->second;
      return this->checker.genericChecker().forceConversion(
          this->checkExprKind(expr, expected, wraps), to);
    }
  }

  return this->checkExprKind(expr, expected, wraps);
}

Expr *ExprChecker::checkExprKind(Expr *expr, const Type *expected, bool wraps) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (FuncSymbol *generic = this->checker.genericChecker().genericNamed(name)) {
      const Type *function = expected ? expected->nonOptional() : nullptr;
      Checker::Signature sig = function && function->isFunction()
                                   ? ClosureChecker::signatureOf(function)
                                   : Checker::Signature();
      return this->checker.genericInstantiator().checkGenericValue(
          name, generic, function && function->isFunction() ? &sig : nullptr);
    }
  }
  if (expected && (isa<NameExpr>(expr) || isa<MemberExpr>(expr))) {
    if (const Type *function = expected->nonOptional(); function->isFunction()) {
      Checker::Signature sig = ClosureChecker::signatureOf(function);
      if (FunctionExpr *wrapper = this->checker.functionValueChecker().builtinFunctionValue(
              expr, sig, unsigned(sig.params.size()), {})) {
        return this->checker.closureChecker().checkFunctionExpr(wrapper, &sig);
      }
    }
  }
  switch (expr->kind()) {
    case Expr::Kind::IntLit: {
      auto *lit = cast<IntLitExpr>(expr);
      LiteralChecker::ResultIntLiteralTyped typed =
          this->checker.literalChecker().checkIntLit(lit, expected, false);
      if (typed.isErr()) {
        this->checker.diags_.error(lit->loc(), typed.error().message);
        return lit;
      }
      return typed.value();
    }
  case Expr::Kind::FloatLit: {
    if (const Type *literal = this->checker.literalTypes().numberLiteralFor(
            expected, cast<FloatLitExpr>(expr)->value, true)) {
      expr->type = literal;

      return expr;
    }
    const Type *member = LiteralTypes::numberMemberOf(expected, true);
    if (expected && (expected->nonOptional()->isDecimal() || (member && member->isDecimal()))) {
      expr->type = Type::decimalTy();
      this->checker.decimalChecker().checkDecimalLiteralRange(expr);
      return expr;
    }
    expr->type = Type::doubleTy();
    return expr;
  }
  case Expr::Kind::BoolLit: {
    const Type *literal =
        this->checker.literalTypes().boolLiteralFor(expected, cast<BoolLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::boolTy();
    return expr;
  }
  case Expr::Kind::StringLit: {
    const Type *literal =
        this->checker.literalTypes().stringLiteralFor(expected, cast<StringLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::string();
    return expr;
  }
  case Expr::Kind::RegexLit:
    return this->checker.regexChecker().checkRegexLit(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    expr->type = Type::undefinedTy();
    return expr;
  case Expr::Kind::Template:
    return this->checker.literalChecker().checkTemplate(cast<TemplateExpr>(expr), expected);
  case Expr::Kind::ArrayLit:
    return this->checker.arrayLiteralChecker().checkArrayLit(cast<ArrayLitExpr>(expr), expected);
  case Expr::Kind::ObjectLit:
    return this->checker.objectLiteralChecker().checkObjectLit(cast<ObjectLitExpr>(expr), expected);
  case Expr::Kind::Name:
    if (Expr *null = this->checker.jsonChecker().checkNull(cast<NameExpr>(expr), expected)) {
      return null;
    }
    if (Expr *members = this->checker.objectChecker().enumObject(cast<NameExpr>(expr), expected)) {
      return members;
    }
    return this->checker.literalTypes().constText(
        this->checker.nameChecker().checkName(cast<NameExpr>(expr)), expected);
  case Expr::Kind::Member:
    return this->checker.memberChecker().checkMember(cast<MemberExpr>(expr), expected);
  case Expr::Kind::Index:
    return this->checker.memberChecker().checkIndex(cast<IndexExpr>(expr));
  case Expr::Kind::Unary:
    return this->checker.unaryChecker().checkUnary(cast<UnaryExpr>(expr), expected, wraps);
  case Expr::Kind::Binary:
    return this->checker.binaryChecker().checkBinary(cast<BinaryExpr>(expr), expected, wraps);
  case Expr::Kind::Assign:
    return this->checker.assignChecker().checkAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return this->checker.assignChecker().checkUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return this->checker.callChecker().checkCall(cast<CallExpr>(expr), expected);
  case Expr::Kind::New:
    return this->checker.newChecker().checkNew(cast<NewExpr>(expr), expected);
  case Expr::Kind::Class:
    return this->checker.lateClasses().checkClassExpr(cast<ClassExpr>(expr), expected);
  case Expr::Kind::Conditional: {
    auto *conditional = cast<ConditionalExpr>(expr);
    ResultBranchesMatch matched = this->checkConditional(conditional, expected);
    if (matched.isErr()) {
      this->checker.diags_.error(conditional->loc(), matched.error().message);
      return conditional;
    }
    return matched.value();
  }
  case Expr::Kind::Cast: {
    auto *castExpr = cast<CastExpr>(expr);
    ResultCastSupported supported = this->checkCast(castExpr);
    if (supported.isErr()) {
      this->checker.diags_.error(castExpr->loc(), supported.error().message);
      return castExpr;
    }
    return supported.value();
  }
  case Expr::Kind::Sequence:
    return this->checkSequence(cast<SequenceExpr>(expr), expected, wraps);
  case Expr::Kind::OptionalChain:
    return this->checkOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Function: {
    const Type *function = expected ? expected->nonOptional() : nullptr;
    if (function && function->isFunction()) {
      Checker::Signature sig = ClosureChecker::signatureOf(function);
      return this->checker.closureChecker().checkFunctionExpr(cast<FunctionExpr>(expr), &sig);
    }
    return this->checker.closureChecker().checkFunctionExpr(cast<FunctionExpr>(expr), nullptr);
  }
  case Expr::Kind::Yield:
    return this->checker.generatorChecker().checkYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return this->checker.asyncChecker().checkAwait(cast<AwaitExpr>(expr), expected);
  }
  llvm_unreachable("unknown expression kind");
}

Expr *ExprChecker::checkSequence(SequenceExpr *seq, const Type *expected, bool wraps) {
  for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
    seq->exprs[i] = this->checkExpr(seq->exprs[i]);
    this->checker.resultChecker().checkDiscarded(seq->exprs[i]);
  }
  Expr *&last = seq->exprs.back();
  last = this->checkExpr(last, expected, wraps);
  seq->type = last->type;

  return seq;
}

Expr *ExprChecker::checkOptionalChain(OptionalChainExpr *chain) {
  bool outer = std::exchange(this->checker.chainShortCircuits_, false);
  Checker::Flow before = this->checker.flow_;
  chain->chain = this->checkExpr(chain->chain);
  chain->mayShortCircuit = this->checker.chainShortCircuits_;
  this->checker.chainShortCircuits_ = outer;
  const Type *type = chain->chain->type;
  chain->type = type;
  if (!chain->mayShortCircuit || type->isError()) {
    return chain;
  }
  this->checker.flow_ = FlowChecker::join(before, this->checker.flow_);
  if (!type->isVoid()) {
    chain->type = Type::optional(type);
  }

  return chain;
}

Expr *ExprChecker::chainLink(Expr *object) {
  const Type *type = object->type;
  if (type->nonOptional()->isJson()) {
    auto *link = this->checker.ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
    link->type = Type::json();
    link->shortCircuit = true;
    this->checker.chainShortCircuits_ = true;
    return link;
  }
  if (!type->isOptional()) {
    return object;
  }
  auto *link = this->checker.ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type->inner();
  link->shortCircuit = true;
  this->checker.chainShortCircuits_ = true;

  return link;
}

ExprChecker::ResultBranchesMatch ExprChecker::checkConditional(ConditionalExpr *cond,
                                                               const Type *expected) {
  Checker::Flow whenTrue, whenFalse;
  cond->cond = this->checker.truthChecker().checkCondition(cond->cond, whenTrue, whenFalse);
  if (std::optional<bool> constant =
          this->checker.checkingInstance_
              ? this->checker.genericChecker().constantCondition(cond->cond)
              : std::nullopt) {
    this->checker.flow_ = *constant ? whenTrue : whenFalse;
    Expr *chosen = this->checkExpr(*constant ? cond->thenExpr : cond->elseExpr, expected);
    return ok(chosen);
  }
  auto waits = [&](const Expr *branch) {
    return Destination::literalShaped(branch) || DecimalChecker::isLiteralTree(branch);
  };
  auto hint = [&](const Expr *literal, const Expr *other) {
    if (DecimalChecker::isLiteralTree(literal)) {
      return DecimalChecker::literalHint(other, expected);
    }

    return expected ? expected : other->type->isError() ? nullptr : other->type;
  };
  bool thenWaits = waits(cond->thenExpr);
  bool elseWaits = waits(cond->elseExpr);
  if (thenWaits && !elseWaits) {
    this->checker.flow_ = whenFalse;
    cond->elseExpr = this->checkExpr(cond->elseExpr, expected);
    Checker::Flow afterElse = this->checker.flow_;
    this->checker.flow_ = whenTrue;
    cond->thenExpr = this->checkExpr(cond->thenExpr, hint(cond->thenExpr, cond->elseExpr));
    this->checker.flow_ = FlowChecker::join(this->checker.flow_, afterElse);
  } else {
    this->checker.flow_ = whenTrue;
    cond->thenExpr = this->checkExpr(cond->thenExpr, expected);
    Checker::Flow afterThen = this->checker.flow_;
    this->checker.flow_ = whenFalse;
    const Type *elseHint = expected;
    if (elseWaits && !thenWaits) {
      elseHint = hint(cond->elseExpr, cond->thenExpr);
    } else if (DecimalChecker::isLiteralTree(cond->elseExpr)) {
      elseHint = DecimalChecker::literalHint(cond->thenExpr, expected);
    }
    cond->elseExpr = this->checkExpr(cond->elseExpr, elseHint);
    this->checker.flow_ = FlowChecker::join(afterThen, this->checker.flow_);
  }

  const Type *a = cond->thenExpr->type;
  const Type *b = cond->elseExpr->type;
  cond->type = Type::error();
  if (a->isError() || b->isError()) {
    if (this->checker.trial_ && a->isError() != b->isError()) {
      cond->type = a->isError() ? b : a;
    }
    return ok(cond);
  }
  if (a->isVoid() && b->isVoid()) {
    cond->type = a;
    return ok(cond);
  }
  if (a != b && this->checker.typeRelations().literalAdapts(cond->thenExpr, b)) {
    cond->thenExpr = this->checker.typeRelations().adaptLiteral(cond->thenExpr, b);
    a = b;
  } else if (a != b && this->checker.typeRelations().literalAdapts(cond->elseExpr, a)) {
    cond->elseExpr = this->checker.typeRelations().adaptLiteral(cond->elseExpr, a);
    b = a;
  }
  const Type *common = LiteralTypes::unionUnify(a, b);
  if (!common) {
    std::string message = ErrorTextConditional::mismatch(a->name(), b->name());

    return err(ExprCheckerCheckConditionalError{
        .code = ExprCheckerCheckConditionalErrorCode::INCOMPATIBLE_BRANCHES,
        .message = message,
    });
  }
  cond->thenExpr = this->checker.typeRelations().convert(cond->thenExpr, common);
  cond->elseExpr = this->checker.typeRelations().convert(cond->elseExpr, common);
  cond->type = common;

  return ok(cond);
}

ExprChecker::ResultCastSupported ExprChecker::checkCast(CastExpr *cast) {
  const Type *to = this->checker.typeResolver().resolveType(*cast->target, false);
  bool arrayLiteral = isa<ArrayLitExpr>(cast->operand);
  cast->operand = this->checkExpr(cast->operand, arrayLiteral && !to->isError() ? to : nullptr);
  const Type *from = cast->operand->type;
  cast->type = to;
  if (from->isError() || to->isError()) {
    std::string message = ErrorTextCast::plain();

    return err(ExprCheckerCheckCastError{
        .code = ExprCheckerCheckCastErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }
  if (arrayLiteral) {
    std::string message = ErrorTextCast::declare(to->name());

    return err(ExprCheckerCheckCastError{
        .code = ExprCheckerCheckCastErrorCode::ARRAY_LITERAL,
        .message = message,
    });
  }
  if (from == to || TypeRelations::isImplicitlyConvertible(from, to)) {
    std::string message = ErrorTextCast::itself(to->name());

    return err(ExprCheckerCheckCastError{
        .code = ExprCheckerCheckCastErrorCode::CONVERTS_BY_ITSELF,
        .message = message,
    });
  }
  if (from->isOptional() && TypeRelations::isImplicitlyConvertible(from->inner(), to)) {
    std::string message = ErrorTextCast::undefined();

    return err(ExprCheckerCheckCastError{
        .code = ExprCheckerCheckCastErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  std::string hint = TypeRelations::conversionHint(cast->operand, to);
  if (!hint.empty()) {
    std::string message = ErrorTextCast::hinted(hint);

    return err(ExprCheckerCheckCastError{
        .code = ExprCheckerCheckCastErrorCode::NEEDS_CONVERSION,
        .message = message,
    });
  }
  if (from->isFloat() && to->isDecimal()) {
    std::string message = ErrorTextCast::toDecimal();

    return err(ExprCheckerCheckCastError{
        .code = ExprCheckerCheckCastErrorCode::DOUBLE_TO_DECIMAL,
        .message = message,
    });
  }
  if (from->isDecimal() && to->isNumeric()) {
    std::string message = ErrorTextCast::toNumber();

    return err(ExprCheckerCheckCastError{
        .code = ExprCheckerCheckCastErrorCode::DECIMAL_TO_NUMBER,
        .message = message,
    });
  }
  if (from->nonOptional()->isJson()) {
    std::string message = ErrorTextCast::json();

    return err(ExprCheckerCheckCastError{
        .code = ExprCheckerCheckCastErrorCode::JSON_VALUE,
        .message = message,
    });
  }

  std::string message = ErrorTextCast::cannot(from->name(), to->name());

  return err(ExprCheckerCheckCastError{
      .code = ExprCheckerCheckCastErrorCode::CANNOT_BECOME,
      .message = message,
  });
}

}
