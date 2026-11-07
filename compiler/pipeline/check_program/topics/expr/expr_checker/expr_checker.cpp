#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/topics/async/async_checker/async_checker.h"
#include "compiler/pipeline/check_program/topics/decimal/decimal_checker/decimal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/assign_checker/assign_checker.h"
#include "compiler/pipeline/check_program/topics/expr/binary_checker/binary_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/literal_checker/literal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker.h"
#include "compiler/pipeline/check_program/topics/expr/name_checker/name_checker.h"
#include "compiler/pipeline/check_program/topics/expr/new_checker/new_checker.h"
#include "compiler/pipeline/check_program/topics/expr/truth_checker/truth_checker.h"
#include "compiler/pipeline/check_program/topics/expr/unary_checker/unary_checker.h"
#include "compiler/pipeline/check_program/topics/functions/closure_checker/closure_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_value_checker/function_value_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/generator_checker/generator_checker.h"
#include "compiler/pipeline/check_program/topics/json/json_checker/json_checker.h"
#include "compiler/pipeline/check_program/topics/objects/object_checker/object_checker.h"
#include "compiler/pipeline/check_program/topics/objects/object_literal_checker/object_literal_checker.h"
#include "compiler/pipeline/check_program/topics/regex/regex_checker/regex_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_checker/result_checker.h"
#include "compiler/pipeline/check_program/topics/values/array_literal_checker/array_literal_checker.h"
#include "compiler/pipeline/check_program/types/destination/destination.h"
#include "compiler/pipeline/check_program/types/literal_types/literal_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ExprChecker::ExprChecker(CheckState &owner)
    : state(owner) {}

Expr *ExprChecker::checkExpr(Expr *expr, const Type *expected, bool wraps) {
  GenericStore &generics = this->state.getGenericStore();
  if (generics.isCheckingInstance() && generics.hasForcedTypes()) {
    if (const Type *to = generics.findForcedType(expr)) {
      return GenericChecker(this->state)
          .forceConversion(this->checkExprKind(expr, expected, wraps), to);
    }
  }

  return this->checkExprKind(expr, expected, wraps);
}

Expr *ExprChecker::checkExprKind(Expr *expr, const Type *expected, bool wraps) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (FuncSymbol *generic = GenericChecker(this->state).genericNamed(name)) {
      const Type *function = expected ? expected->nonOptional() : nullptr;
      Signature sig =
          function && function->isFunction() ? ClosureChecker::signatureOf(function) : Signature();
      return GenericInstantiator(this->state)
          .checkGenericValue(name, generic, function && function->isFunction() ? &sig : nullptr);
    }
  }
  if (expected && (isa<NameExpr>(expr) || isa<MemberExpr>(expr))) {
    if (const Type *function = expected->nonOptional(); function->isFunction()) {
      Signature sig = ClosureChecker::signatureOf(function);
      if (FunctionExpr *wrapper =
              FunctionValueChecker(this->state)
                  .builtinFunctionValue(expr, sig, unsigned(sig.params.size()), {})) {
        return ClosureChecker(this->state).checkFunctionExpr(wrapper, &sig);
      }
    }
  }
  switch (expr->kind()) {
    case Expr::Kind::IntLit: {
      auto *lit = cast<IntLitExpr>(expr);
      LiteralChecker::ResultIntLiteralTyped typed =
          LiteralChecker(this->state).checkIntLit(lit, expected, false);
      if (typed.isErr()) {
        this->state.getDiagnostics().error(lit->loc(), typed.error().message);
        return lit;
      }
      return typed.value();
    }
  case Expr::Kind::FloatLit: {
    if (const Type *literal =
            LiteralTypes(this->state)
                .numberLiteralFor(expected, cast<FloatLitExpr>(expr)->value, true)) {
      expr->type = literal;

      return expr;
    }
    const Type *member = LiteralTypes::numberMemberOf(expected, true);
    if (expected && (expected->nonOptional()->isDecimal() || (member && member->isDecimal()))) {
      expr->type = Type::decimalTy();
      DecimalChecker(this->state).checkDecimalLiteralRange(expr);
      return expr;
    }
    expr->type = Type::doubleTy();
    return expr;
  }
  case Expr::Kind::BoolLit: {
    const Type *literal =
        LiteralTypes(this->state).boolLiteralFor(expected, cast<BoolLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::boolTy();
    return expr;
  }
  case Expr::Kind::StringLit: {
    const Type *literal =
        LiteralTypes(this->state).stringLiteralFor(expected, cast<StringLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::string();
    return expr;
  }
  case Expr::Kind::RegexLit:
    return RegexChecker(this->state).checkRegexLit(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    expr->type = Type::undefinedTy();
    return expr;
  case Expr::Kind::Template:
    return LiteralChecker(this->state).checkTemplate(cast<TemplateExpr>(expr), expected);
  case Expr::Kind::ArrayLit:
    return ArrayLiteralChecker(this->state).checkArrayLit(cast<ArrayLitExpr>(expr), expected);
  case Expr::Kind::ObjectLit:
    return ObjectLiteralChecker(this->state).checkObjectLit(cast<ObjectLitExpr>(expr), expected);
  case Expr::Kind::Name:
    if (Expr *null = JsonChecker(this->state).checkNull(cast<NameExpr>(expr), expected)) {
      return null;
    }
    if (Expr *members = ObjectChecker(this->state).enumObject(cast<NameExpr>(expr), expected)) {
      return members;
    }
    return LiteralTypes(this->state)
        .constText(NameChecker(this->state).checkName(cast<NameExpr>(expr)), expected);
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    MemberChecker::ResultMemberReadable read =
        MemberChecker(this->state).checkMember(member, expected);
    if (read.isOk()) {
      return read.value();
    }
    bool onObject = read.error().code == MemberCheckerCheckMemberErrorCode::MAY_BE_UNDEFINED ||
                    read.error().code == MemberCheckerCheckMemberErrorCode::UNKNOWN_NOT_NARROWED;
    llvm::SMLoc at = onObject ? member->object->loc() : member->loc();
    this->state.getDiagnostics().error(at, read.error().message);
    return member;
  }
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    MemberChecker::ResultIndexedElement element = MemberChecker(this->state).checkIndex(index);
    if (element.isOk()) {
      return element.value();
    }
    llvm::SMLoc at = index->loc();
    switch (element.error().code) {
      case MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED:
        return index;
      case MemberCheckerCheckIndexErrorCode::ENUM_NOT_INTEGER:
      case MemberCheckerCheckIndexErrorCode::GENERIC_INDEX:
      case MemberCheckerCheckIndexErrorCode::KEY_REJECTED:
        at = index->index->loc();
        break;
      case MemberCheckerCheckIndexErrorCode::UNKNOWN_NOT_NARROWED:
      case MemberCheckerCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED:
        at = index->object->loc();
        break;
      case MemberCheckerCheckIndexErrorCode::ENUM_STRING:
      case MemberCheckerCheckIndexErrorCode::FIELD_REJECTED:
      case MemberCheckerCheckIndexErrorCode::DICT_INDEX:
      case MemberCheckerCheckIndexErrorCode::NOT_INDEXABLE:
        break;
    }
    this->state.getDiagnostics().error(at, element.error().message);
    return index;
  }
  case Expr::Kind::Unary:
    return UnaryChecker(this->state).checkUnary(cast<UnaryExpr>(expr), expected, wraps);
  case Expr::Kind::Binary:
    return BinaryChecker(this->state).checkBinary(cast<BinaryExpr>(expr), expected, wraps);
  case Expr::Kind::Assign:
    return AssignChecker(this->state).checkAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return AssignChecker(this->state).checkUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return CallChecker(this->state).checkCall(cast<CallExpr>(expr), expected);
  case Expr::Kind::New:
    return NewChecker(this->state).checkNew(cast<NewExpr>(expr), expected);
  case Expr::Kind::Class:
    return LateClasses(this->state).checkClassExpr(cast<ClassExpr>(expr), expected);
  case Expr::Kind::Conditional: {
    auto *conditional = cast<ConditionalExpr>(expr);
    ResultBranchesMatch matched = this->checkConditional(conditional, expected);
    if (matched.isErr()) {
      this->state.getDiagnostics().error(conditional->loc(), matched.error().message);
      return conditional;
    }
    return matched.value();
  }
  case Expr::Kind::Cast: {
    auto *castExpr = cast<CastExpr>(expr);
    ResultCastSupported supported = this->checkCast(castExpr);
    if (supported.isErr()) {
      this->state.getDiagnostics().error(castExpr->loc(), supported.error().message);
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
      Signature sig = ClosureChecker::signatureOf(function);
      return ClosureChecker(this->state).checkFunctionExpr(cast<FunctionExpr>(expr), &sig);
    }
    return ClosureChecker(this->state).checkFunctionExpr(cast<FunctionExpr>(expr), nullptr);
  }
  case Expr::Kind::Yield:
    return GeneratorChecker(this->state).checkYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return AsyncChecker(this->state).checkAwait(cast<AwaitExpr>(expr), expected);
  }
  llvm_unreachable("unknown expression kind");
}

Expr *ExprChecker::checkSequence(SequenceExpr *seq, const Type *expected, bool wraps) {
  for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
    seq->exprs[i] = this->checkExpr(seq->exprs[i]);
    ResultChecker(this->state).checkDiscarded(seq->exprs[i]);
  }
  Expr *&last = seq->exprs.back();
  last = this->checkExpr(last, expected, wraps);
  seq->type = last->type;

  return seq;
}

Expr *ExprChecker::checkOptionalChain(OptionalChainExpr *chain) {
  this->state.getFunctionStack().enterOptionalChain();
  Flow before = this->state.getFlowState().getFlow();
  chain->chain = this->checkExpr(chain->chain);
  chain->mayShortCircuit = this->state.getFunctionStack().isShortCircuiting();
  this->state.getFunctionStack().leaveOptionalChain();
  const Type *type = chain->chain->type;
  chain->type = type;
  if (!chain->mayShortCircuit || type->isError()) {
    return chain;
  }
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(before, this->state.getFlowState().getFlow()));
  if (!type->isVoid()) {
    chain->type = Type::optional(type);
  }

  return chain;
}

Expr *ExprChecker::chainLink(Expr *object) {
  const Type *type = object->type;
  if (type->nonOptional()->isJson()) {
    auto *link = this->state.getAst().make<CastExpr>(object->loc(), object, std::nullopt);
    link->type = Type::json();
    link->shortCircuit = true;
    this->state.getFunctionStack().markShortCircuit();
    return link;
  }
  if (!type->isOptional()) {
    return object;
  }
  auto *link = this->state.getAst().make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type->inner();
  link->shortCircuit = true;
  this->state.getFunctionStack().markShortCircuit();

  return link;
}

ExprChecker::ResultBranchesMatch ExprChecker::checkConditional(ConditionalExpr *cond,
                                                               const Type *expected) {
  Flow whenTrue, whenFalse;
  cond->cond = TruthChecker(this->state).checkCondition(cond->cond, whenTrue, whenFalse);
  if (std::optional<bool> constant = this->state.getGenericStore().isCheckingInstance()
                                         ? GenericChecker(this->state).constantCondition(cond->cond)
                                         : std::nullopt) {
    this->state.getFlowState().replaceFlow(*constant ? whenTrue : whenFalse);
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
    this->state.getFlowState().replaceFlow(whenFalse);
    cond->elseExpr = this->checkExpr(cond->elseExpr, expected);
    Flow afterElse = this->state.getFlowState().getFlow();
    this->state.getFlowState().replaceFlow(whenTrue);
    cond->thenExpr = this->checkExpr(cond->thenExpr, hint(cond->thenExpr, cond->elseExpr));
    this->state.getFlowState().replaceFlow(
        FlowChecker::join(this->state.getFlowState().getFlow(), afterElse));
  } else {
    this->state.getFlowState().replaceFlow(whenTrue);
    cond->thenExpr = this->checkExpr(cond->thenExpr, expected);
    Flow afterThen = this->state.getFlowState().getFlow();
    this->state.getFlowState().replaceFlow(whenFalse);
    const Type *elseHint = expected;
    if (elseWaits && !thenWaits) {
      elseHint = hint(cond->elseExpr, cond->thenExpr);
    } else if (DecimalChecker::isLiteralTree(cond->elseExpr)) {
      elseHint = DecimalChecker::literalHint(cond->thenExpr, expected);
    }
    cond->elseExpr = this->checkExpr(cond->elseExpr, elseHint);
    this->state.getFlowState().replaceFlow(
        FlowChecker::join(afterThen, this->state.getFlowState().getFlow()));
  }

  const Type *a = cond->thenExpr->type;
  const Type *b = cond->elseExpr->type;
  cond->type = Type::error();
  if (a->isError() || b->isError()) {
    if (this->state.getFunctionStack().isInsideTrial() && a->isError() != b->isError()) {
      cond->type = a->isError() ? b : a;
    }
    return ok(cond);
  }
  if (a->isVoid() && b->isVoid()) {
    cond->type = a;
    return ok(cond);
  }
  if (a != b && TypeRelations(this->state).literalAdapts(cond->thenExpr, b)) {
    cond->thenExpr = TypeRelations(this->state).adaptLiteral(cond->thenExpr, b);
    a = b;
  } else if (a != b && TypeRelations(this->state).literalAdapts(cond->elseExpr, a)) {
    cond->elseExpr = TypeRelations(this->state).adaptLiteral(cond->elseExpr, a);
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
  cond->thenExpr = TypeRelations(this->state).convert(cond->thenExpr, common);
  cond->elseExpr = TypeRelations(this->state).convert(cond->elseExpr, common);
  cond->type = common;

  return ok(cond);
}

ExprChecker::ResultCastSupported ExprChecker::checkCast(CastExpr *cast) {
  const Type *to = TypeResolver(this->state).resolveType(*cast->target, false);
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
