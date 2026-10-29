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
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ExprChecker::ExprChecker(Checker &checker) : checker_(checker) {}

Expr *ExprChecker::checkExpr(Expr *expr, const Type *expected, bool wraps) {
  if (checker_.checkingInstance_ && !checker_.forced_.empty()) {
    auto found = checker_.forced_.find({expr->loc().getPointer(), unsigned(expr->kind())});
    if (found != checker_.forced_.end()) {
      const Type *to = found->second;
      return checker_.genericChecker().forceConversion(checkExprKind(expr, expected, wraps), to);
    }
  }

  return checkExprKind(expr, expected, wraps);
}

Expr *ExprChecker::checkExprKind(Expr *expr, const Type *expected, bool wraps) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (FuncSymbol *generic = checker_.genericChecker().genericNamed(name)) {
      const Type *function = expected ? expected->nonOptional() : nullptr;
      Checker::Signature sig = function && function->isFunction()
                                   ? ClosureChecker::signatureOf(function)
                                   : Checker::Signature();
      return checker_.genericInstantiator().checkGenericValue(
          name, generic, function && function->isFunction() ? &sig : nullptr);
    }
  }
  if (expected && (isa<NameExpr>(expr) || isa<MemberExpr>(expr))) {
    if (const Type *function = expected->nonOptional(); function->isFunction()) {
      Checker::Signature sig = ClosureChecker::signatureOf(function);
      if (FunctionExpr *wrapper = checker_.functionValueChecker().builtinFunctionValue(
              expr, sig, unsigned(sig.params.size()), {})) {
        return checker_.closureChecker().checkFunctionExpr(wrapper, &sig);
      }
    }
  }
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
    return checker_.literalChecker().checkIntLit(cast<IntLitExpr>(expr), expected, false);
  case Expr::Kind::FloatLit: {
    if (const Type *literal = checker_.literalTypes().numberLiteralFor(
            expected, cast<FloatLitExpr>(expr)->value, true)) {
      expr->type = literal;

      return expr;
    }
    const Type *member = LiteralTypes::numberMemberOf(expected, true);
    if (expected && (expected->nonOptional()->isDecimal() || (member && member->isDecimal()))) {
      expr->type = Type::decimalTy();
      checker_.decimalChecker().checkDecimalLiteralRange(expr);
      return expr;
    }
    expr->type = Type::doubleTy();
    return expr;
  }
  case Expr::Kind::BoolLit: {
    const Type *literal =
        checker_.literalTypes().boolLiteralFor(expected, cast<BoolLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::boolTy();
    return expr;
  }
  case Expr::Kind::StringLit: {
    const Type *literal =
        checker_.literalTypes().stringLiteralFor(expected, cast<StringLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::string();
    return expr;
  }
  case Expr::Kind::RegexLit:
    return checker_.regexChecker().checkRegexLit(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    expr->type = Type::undefinedTy();
    return expr;
  case Expr::Kind::Template:
    return checker_.literalChecker().checkTemplate(cast<TemplateExpr>(expr), expected);
  case Expr::Kind::ArrayLit:
    return checker_.arrayLiteralChecker().checkArrayLit(cast<ArrayLitExpr>(expr), expected);
  case Expr::Kind::ObjectLit:
    return checker_.objectLiteralChecker().checkObjectLit(cast<ObjectLitExpr>(expr), expected);
  case Expr::Kind::Name:
    if (Expr *null = checker_.jsonChecker().checkNull(cast<NameExpr>(expr), expected)) {
      return null;
    }
    return checker_.nameChecker().checkName(cast<NameExpr>(expr));
  case Expr::Kind::Member:
    return checker_.memberChecker().checkMember(cast<MemberExpr>(expr), expected);
  case Expr::Kind::Index:
    return checker_.memberChecker().checkIndex(cast<IndexExpr>(expr));
  case Expr::Kind::Unary:
    return checker_.unaryChecker().checkUnary(cast<UnaryExpr>(expr), expected, wraps);
  case Expr::Kind::Binary:
    return checker_.binaryChecker().checkBinary(cast<BinaryExpr>(expr), expected, wraps);
  case Expr::Kind::Assign:
    return checker_.assignChecker().checkAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return checker_.assignChecker().checkUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return checker_.callChecker().checkCall(cast<CallExpr>(expr), expected);
  case Expr::Kind::New:
    return checker_.newChecker().checkNew(cast<NewExpr>(expr), expected);
  case Expr::Kind::Class:
    return checker_.lateClasses().checkClassExpr(cast<ClassExpr>(expr), expected);
  case Expr::Kind::Conditional:
    return checkConditional(cast<ConditionalExpr>(expr), expected);
  case Expr::Kind::Cast:
    return checkCast(cast<CastExpr>(expr));
  case Expr::Kind::Sequence:
    return checkSequence(cast<SequenceExpr>(expr), expected, wraps);
  case Expr::Kind::OptionalChain:
    return checkOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Function: {
    const Type *function = expected ? expected->nonOptional() : nullptr;
    if (function && function->isFunction()) {
      Checker::Signature sig = ClosureChecker::signatureOf(function);
      return checker_.closureChecker().checkFunctionExpr(cast<FunctionExpr>(expr), &sig);
    }
    return checker_.closureChecker().checkFunctionExpr(cast<FunctionExpr>(expr), nullptr);
  }
  case Expr::Kind::Yield:
    return checker_.generatorChecker().checkYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return checker_.asyncChecker().checkAwait(cast<AwaitExpr>(expr), expected);
  }
  llvm_unreachable("unknown expression kind");
}

Expr *ExprChecker::checkSequence(SequenceExpr *seq, const Type *expected, bool wraps) {
  for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
    seq->exprs[i] = checkExpr(seq->exprs[i]);
    checker_.resultChecker().checkDiscarded(seq->exprs[i]);
  }
  Expr *&last = seq->exprs.back();
  last = checkExpr(last, expected, wraps);
  seq->type = last->type;

  return seq;
}

Expr *ExprChecker::checkOptionalChain(OptionalChainExpr *chain) {
  bool outer = std::exchange(checker_.chainShortCircuits_, false);
  Checker::Flow before = checker_.flow_;
  chain->chain = checkExpr(chain->chain);
  chain->mayShortCircuit = checker_.chainShortCircuits_;
  checker_.chainShortCircuits_ = outer;
  const Type *type = chain->chain->type;
  chain->type = type;
  if (!chain->mayShortCircuit || type->isError()) {
    return chain;
  }
  checker_.flow_ = FlowChecker::join(before, checker_.flow_);
  if (!type->isVoid()) {
    chain->type = Type::optional(type);
  }

  return chain;
}

Expr *ExprChecker::chainLink(Expr *object) {
  const Type *type = object->type;
  if (type->nonOptional()->isJson()) {
    auto *link = checker_.ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
    link->type = Type::json();
    link->shortCircuit = true;
    checker_.chainShortCircuits_ = true;
    return link;
  }
  if (!type->isOptional()) {
    return object;
  }
  auto *link = checker_.ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type->inner();
  link->shortCircuit = true;
  checker_.chainShortCircuits_ = true;

  return link;
}

Expr *ExprChecker::checkConditional(ConditionalExpr *cond, const Type *expected) {
  Checker::Flow whenTrue, whenFalse;
  cond->cond = checker_.truthChecker().checkCondition(cond->cond, whenTrue, whenFalse);
  if (std::optional<bool> constant = checker_.checkingInstance_
                                         ? checker_.genericChecker().constantCondition(cond->cond)
                                         : std::nullopt) {
    checker_.flow_ = *constant ? whenTrue : whenFalse;
    return checkExpr(*constant ? cond->thenExpr : cond->elseExpr, expected);
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
  bool thenWaits = waits(cond->thenExpr), elseWaits = waits(cond->elseExpr);
  if (thenWaits && !elseWaits) {
    checker_.flow_ = whenFalse;
    cond->elseExpr = checkExpr(cond->elseExpr, expected);
    Checker::Flow afterElse = checker_.flow_;
    checker_.flow_ = whenTrue;
    cond->thenExpr = checkExpr(cond->thenExpr, hint(cond->thenExpr, cond->elseExpr));
    checker_.flow_ = FlowChecker::join(checker_.flow_, afterElse);
  } else {
    checker_.flow_ = whenTrue;
    cond->thenExpr = checkExpr(cond->thenExpr, expected);
    Checker::Flow afterThen = checker_.flow_;
    checker_.flow_ = whenFalse;
    const Type *elseHint = expected;
    if (elseWaits && !thenWaits) {
      elseHint = hint(cond->elseExpr, cond->thenExpr);
    } else if (DecimalChecker::isLiteralTree(cond->elseExpr)) {
      elseHint = DecimalChecker::literalHint(cond->thenExpr, expected);
    }
    cond->elseExpr = checkExpr(cond->elseExpr, elseHint);
    checker_.flow_ = FlowChecker::join(afterThen, checker_.flow_);
  }

  const Type *a = cond->thenExpr->type, *b = cond->elseExpr->type;
  cond->type = Type::error();
  if (a->isError() || b->isError()) {
    if (checker_.trial_ && a->isError() != b->isError()) {
      cond->type = a->isError() ? b : a;
    }
    return cond;
  }
  if (a->isVoid() && b->isVoid()) {
    cond->type = a;
    return cond;
  }
  if (a != b && checker_.typeRelations().literalAdapts(cond->thenExpr, b)) {
    cond->thenExpr = checker_.typeRelations().adaptLiteral(cond->thenExpr, b);
    a = b;
  } else if (a != b && checker_.typeRelations().literalAdapts(cond->elseExpr, a)) {
    cond->elseExpr = checker_.typeRelations().adaptLiteral(cond->elseExpr, a);
    b = a;
  }
  const Type *common = LiteralTypes::unionUnify(a, b);
  if (!common) {
    checker_.diags_.error(cond->loc(), "the branches of '?:' have incompatible types '" +
                                           a->name() + "' and '" + b->name() + "'");
    return cond;
  }
  cond->thenExpr = checker_.typeRelations().convert(cond->thenExpr, common);
  cond->elseExpr = checker_.typeRelations().convert(cond->elseExpr, common);
  cond->type = common;

  return cond;
}

Expr *ExprChecker::checkCast(CastExpr *cast) {
  const Type *to = checker_.typeResolver().resolveType(*cast->target, false);
  bool arrayLiteral = isa<ArrayLitExpr>(cast->operand);
  cast->operand = checkExpr(cast->operand, arrayLiteral && !to->isError() ? to : nullptr);
  const Type *from = cast->operand->type;
  cast->type = to;
  std::string message = "'as' is not supported";
  if (from->isError() || to->isError())
    ;
  else if (arrayLiteral) {
    message +=
        "; declare the type of the variable instead, e.g. 'let x: " + to->name().str() + " = ...'";
  } else if (from == to || TypeRelations::isImplicitlyConvertible(from, to)) {
    message += "; the value converts to '" + to->name().str() + "' by itself, so remove 'as'";
  } else if (from->isOptional() && TypeRelations::isImplicitlyConvertible(from->inner(), to)) {
    message += "; the value may be undefined: check it first (e.g. 'x != undefined') or give a "
               "default with '??'";
  } else if (std::string hint = TypeRelations::conversionHint(cast->operand, to); !hint.empty()) {
    message += hint;
  } else if (from->isFloat() && to->isDecimal()) {
    message += "; convert the double with 'decimal(x)'";
  } else if (from->isDecimal() && to->isNumeric()) {
    message += "; 'Number(x)' gives the nearest double";
  } else if (from->nonOptional()->isJson()) {
    message += "; check what a Json holds first, e.g. 'typeof x == \"number\"', and the check "
               "narrows it";
  } else {
    message += ", and type '" + from->name().str() + "' cannot become '" + to->name().str() + "'";
  }
  checker_.diags_.error(cast->loc(), message);

  return cast;
}

}
