#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/unary_checker/unary_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/assign_checker/assign_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/expr/new_checker/new_checker.h"
#include "compiler/check/values/array_literal_checker/array_literal_checker.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/objects/object_literal_checker/object_literal_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
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
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

ExprChecker::ExprChecker(Sema &sema) : sema_(sema) {}

Expr *ExprChecker::checkExpr(Expr *expr, const Type *expected, bool wraps) {
  // In an instance, an expression the generic code converted is converted
  // the same way (see noteForced).
  if (sema_.checkingInstance_ && !sema_.forced_.empty()) {
    auto found = sema_.forced_.find({expr->loc().getPointer(), unsigned(expr->kind())});
    if (found != sema_.forced_.end()) {
      const Type *to = found->second;
      return sema_.genericChecker().forceConversion(checkExprKind(expr, expected, wraps), to);
    }
  }

  return checkExprKind(expr, expected, wraps);
}

Expr *ExprChecker::checkExprKind(Expr *expr, const Type *expected, bool wraps) {
  // A generic function as a value: its type arguments come from the type
  // expected.
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (FuncSymbol *generic = sema_.genericChecker().genericNamed(name)) {
      const Type *function = expected ? expected->nonOptional() : nullptr;
      Sema::Signature sig = function && function->isFunction() ? ClosureChecker::signatureOf(function) : Sema::Signature();
      return sema_.genericInstantiator().checkGenericValue(name, generic, function && function->isFunction() ? &sig : nullptr);
    }
  }
  // A built-in such as Math.sqrt where a function is expected.
  if (expected && (isa<NameExpr>(expr) || isa<MemberExpr>(expr))) {
    if (const Type *function = expected->nonOptional(); function->isFunction()) {
      Sema::Signature sig = ClosureChecker::signatureOf(function);
      if (FunctionExpr *wrapper =
              sema_.functionValueChecker().builtinFunctionValue(expr, sig, unsigned(sig.params.size()), {})) {
        return sema_.closureChecker().checkFunctionExpr(wrapper, &sig);
      }
    }
  }
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
    return sema_.literalChecker().checkIntLit(cast<IntLitExpr>(expr), expected, /*negated=*/false);
  case Expr::Kind::FloatLit: {
    // Where a literal type is expected, the type of its value.
    if (const Type *literal = sema_.literalTypes().numberLiteralFor(expected, cast<FloatLitExpr>(expr)->value, true)) {
      expr->type = literal;

      return expr;
    }
    // Where a decimal is expected, the literal is read exactly.
    const Type *member = LiteralTypes::numberMemberOf(expected, /*fraction=*/true);
    if (expected && (expected->nonOptional()->isDecimal() || (member && member->isDecimal()))) {
      expr->type = Type::decimalTy();
      sema_.decimalChecker().checkDecimalLiteralRange(expr);
      return expr;
    }
    expr->type = Type::doubleTy();
    return expr;
  }
  case Expr::Kind::BoolLit: {
    const Type *literal = sema_.literalTypes().boolLiteralFor(expected, cast<BoolLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::boolTy();
    return expr;
  }
  case Expr::Kind::StringLit: {
    const Type *literal = sema_.literalTypes().stringLiteralFor(expected, cast<StringLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::string();
    return expr;
  }
  case Expr::Kind::RegexLit:
    return sema_.regexChecker().checkRegexLit(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    expr->type = Type::undefinedTy();
    return expr;
  case Expr::Kind::Template:
    return sema_.literalChecker().checkTemplate(cast<TemplateExpr>(expr), expected);
  case Expr::Kind::ArrayLit:
    return sema_.arrayLiteralChecker().checkArrayLit(cast<ArrayLitExpr>(expr), expected);
  case Expr::Kind::ObjectLit:
    return sema_.objectLiteralChecker().checkObjectLit(cast<ObjectLitExpr>(expr), expected);
  case Expr::Kind::Name:
    if (Expr *null = sema_.jsonChecker().checkNull(cast<NameExpr>(expr), expected)) {
      return null;
    }
    return sema_.nameChecker().checkName(cast<NameExpr>(expr));
  case Expr::Kind::Member:
    return sema_.memberChecker().checkMember(cast<MemberExpr>(expr), expected);
  case Expr::Kind::Index:
    return sema_.memberChecker().checkIndex(cast<IndexExpr>(expr));
  case Expr::Kind::Unary:
    return sema_.unaryChecker().checkUnary(cast<UnaryExpr>(expr), expected, wraps);
  case Expr::Kind::Binary:
    return sema_.binaryChecker().checkBinary(cast<BinaryExpr>(expr), expected, wraps);
  case Expr::Kind::Assign:
    return sema_.assignChecker().checkAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return sema_.assignChecker().checkUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return sema_.callChecker().checkCall(cast<CallExpr>(expr), expected);
  case Expr::Kind::New:
    return sema_.newChecker().checkNew(cast<NewExpr>(expr), expected);
  case Expr::Kind::Class:
    return sema_.lateClasses().checkClassExpr(cast<ClassExpr>(expr), expected);
  case Expr::Kind::Conditional:
    return checkConditional(cast<ConditionalExpr>(expr), expected);
  case Expr::Kind::Cast:
    return checkCast(cast<CastExpr>(expr));
  case Expr::Kind::Sequence:
    return checkSequence(cast<SequenceExpr>(expr), expected, wraps);
  case Expr::Kind::OptionalChain:
    return checkOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Function: {
    // Where a function type is expected, the parameters take its types.
    const Type *function = expected ? expected->nonOptional() : nullptr;
    if (function && function->isFunction()) {
      Sema::Signature sig = ClosureChecker::signatureOf(function);
      return sema_.closureChecker().checkFunctionExpr(cast<FunctionExpr>(expr), &sig);
    }
    return sema_.closureChecker().checkFunctionExpr(cast<FunctionExpr>(expr), nullptr);
  }
  case Expr::Kind::Yield:
    return sema_.generatorChecker().checkYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return sema_.asyncChecker().checkAwait(cast<AwaitExpr>(expr), expected);
  }
  llvm_unreachable("unknown expression kind");
}

Expr *ExprChecker::checkSequence(SequenceExpr *seq, const Type *expected, bool wraps) {
  for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
    seq->exprs[i] = checkExpr(seq->exprs[i]);
    sema_.resultChecker().checkDiscarded(seq->exprs[i]);
  }
  Expr *&last = seq->exprs.back();
  last = checkExpr(last, expected, wraps);
  seq->type = last->type;

  return seq;
}

Expr *ExprChecker::checkOptionalChain(OptionalChainExpr *chain) {
  bool outer = std::exchange(sema_.chainShortCircuits_, false);
  Sema::Flow before = sema_.flow_;
  chain->chain = checkExpr(chain->chain);
  chain->mayShortCircuit = sema_.chainShortCircuits_;
  sema_.chainShortCircuits_ = outer;
  const Type *type = chain->chain->type;
  chain->type = type;
  if (!chain->mayShortCircuit || type->isError()) {
    return chain;
  }
  // The rest of the chain may not have run.
  sema_.flow_ = FlowChecker::join(before, sema_.flow_);
  if (!type->isVoid()) {
    chain->type = Type::optional(type);
  }

  return chain;
}

Expr *ExprChecker::chainLink(Expr *object) {
  const Type *type = object->type;
  // A Json that is null ends the chain too, as in JavaScript: `j?.name`.
  if (type->nonOptional()->isJson()) {
    auto *link = sema_.ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
    link->type = Type::json();
    link->shortCircuit = true;
    sema_.chainShortCircuits_ = true;
    return link;
  }
  if (!type->isOptional()) {
    return object; // `?.` on a value that is always there is a plain `.`
  }
  auto *link = sema_.ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type->inner();
  link->shortCircuit = true;
  sema_.chainShortCircuits_ = true;

  return link;
}

Expr *ExprChecker::checkConditional(ConditionalExpr *cond, const Type *expected) {
  Sema::Flow whenTrue, whenFalse;
  cond->cond = sema_.truthChecker().checkCondition(cond->cond, whenTrue, whenFalse);
  // As in checkIf: the branch an instance's types rule out is dropped.
  if (std::optional<bool> constant =
          sema_.checkingInstance_ ? sema_.genericChecker().constantCondition(cond->cond) : std::nullopt) {
    sema_.flow_ = *constant ? whenTrue : whenFalse;
    return checkExpr(*constant ? cond->thenExpr : cond->elseExpr, expected);
  }
  // A literal branch takes the type of the other one, which is checked first
  // then (each branch in its own flow): `flag ? { x: 1 } : p`, and a number
  // literal next to a decimal is a decimal.
  auto waits = [&](const Expr *branch) { return Destination::literalShaped(branch) || DecimalChecker::isLiteralTree(branch); };
  auto hint = [&](const Expr *literal, const Expr *other) {
    if (DecimalChecker::isLiteralTree(literal)) {
      return DecimalChecker::literalHint(other, expected);
    }

    return expected ? expected : other->type->isError() ? nullptr : other->type;
  };
  bool thenWaits = waits(cond->thenExpr), elseWaits = waits(cond->elseExpr);
  if (thenWaits && !elseWaits) {
    sema_.flow_ = whenFalse;
    cond->elseExpr = checkExpr(cond->elseExpr, expected);
    Sema::Flow afterElse = sema_.flow_;
    sema_.flow_ = whenTrue;
    cond->thenExpr = checkExpr(cond->thenExpr, hint(cond->thenExpr, cond->elseExpr));
    sema_.flow_ = FlowChecker::join(sema_.flow_, afterElse);
  } else {
    sema_.flow_ = whenTrue;
    cond->thenExpr = checkExpr(cond->thenExpr, expected);
    Sema::Flow afterThen = sema_.flow_;
    sema_.flow_ = whenFalse;
    const Type *elseHint = expected;
    if (elseWaits && !thenWaits) {
      elseHint = hint(cond->elseExpr, cond->thenExpr);
    } else if (DecimalChecker::isLiteralTree(cond->elseExpr)) {
      elseHint = DecimalChecker::literalHint(cond->thenExpr, expected);
    }
    cond->elseExpr = checkExpr(cond->elseExpr, elseHint);
    sema_.flow_ = FlowChecker::join(afterThen, sema_.flow_);
  }

  const Type *a = cond->thenExpr->type, *b = cond->elseExpr->type;
  cond->type = Type::error();
  if (a->isError() || b->isError()) {
    // A call in a cycle may have no type yet (see inferCycle): the other
    // branch tells.
    if (sema_.trial_ && a->isError() != b->isError()) {
      cond->type = a->isError() ? b : a;
    }
    return cond;
  }
  if (a->isVoid() && b->isVoid()) {
    cond->type = a;
    return cond;
  }
  const Type *common = LiteralTypes::unionUnify(a, b);
  if (!common) {
    sema_.diags_.error(cond->loc(), "the branches of '?:' have incompatible types '" + a->name() +
                                  "' and '" + b->name() + "'");
    return cond;
  }
  cond->thenExpr = sema_.typeRelations().convert(cond->thenExpr, common);
  cond->elseExpr = sema_.typeRelations().convert(cond->elseExpr, common);
  cond->type = common;

  return cond;
}

// `x as T` is not part of the language: in TypeScript it changes nothing at
// run time, so a conversion written with it would behave differently under
// tsc. The error says what to write instead.
Expr *ExprChecker::checkCast(CastExpr *cast) {
  const Type *to = sema_.typeResolver().resolveType(*cast->target, /*allowVoid=*/false);
  // An array literal gets the type it is cast to, so that `[] as int[]`
  // reports only this error.
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
  } else if (from->isFloat() && to->isInt()) {
    message += "; drop the fraction with 'x | 0', or round with Math.round, Math.floor, Math.ceil "
               "or Math.trunc";
  } else if (from->isFloat() && to->isInteger()) {
    message += "; round with Math.round, Math.floor, Math.ceil or Math.trunc where a '" +
               to->name().str() + "' is expected";
  } else if (from->isInteger() && to->isInteger()) {
    message += "; Math.trunc(x) where an 'int' is expected converts it and checks that it fits";
  } else if (from->isFloat() && to->isDecimal()) {
    message += "; convert the double with 'decimal(x)'";
  } else if (from->isDecimal() && to->isNumeric()) {
    message += "; 'Number(x)' gives the nearest double";
  } else if (from->nonOptional()->isJson()) {
    message +=
        "; convert a Json with 'fromJson<" + to->name().str() + ">(x)', which checks what it holds";
  } else {
    message += ", and type '" + from->name().str() + "' cannot become '" + to->name().str() + "'";
  }
  sema_.diags_.error(cast->loc(), message);

  return cast;
}

} // namespace tz
