#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/assign_checker/assign_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/literal_checker/literal_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/expr/name_checker/name_checker.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/expr/unary_checker/unary_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/bodies/objects/object_literal_checker/object_literal_checker.h"
#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ExprChecker::ExprChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->newChecker = &all.getNewChecker();
  this->callChecker = &all.getCallChecker();
  this->jsonChecker = &all.getJsonChecker();
  this->lateClasses = &all.getLateClasses();
  this->nameChecker = &all.getNameChecker();
  this->asyncChecker = &all.getAsyncChecker();
  this->literalTypes = &all.getLiteralTypes();
  this->regexChecker = &all.getRegexChecker();
  this->truthChecker = &all.getTruthChecker();
  this->typeResolver = &all.getTypeResolver();
  this->unaryChecker = &all.getUnaryChecker();
  this->assignChecker = &all.getAssignChecker();
  this->binaryChecker = &all.getBinaryChecker();
  this->memberChecker = &all.getMemberChecker();
  this->objectChecker = &all.getObjectChecker();
  this->resultChecker = &all.getResultChecker();
  this->typeRelations = &all.getTypeRelations();
  this->closureChecker = &all.getClosureChecker();
  this->decimalChecker = &all.getDecimalChecker();
  this->genericChecker = &all.getGenericChecker();
  this->literalChecker = &all.getLiteralChecker();
  this->generatorChecker = &all.getGeneratorChecker();
  this->arrayLiteralChecker = &all.getArrayLiteralChecker();
  this->genericInstantiator = &all.getGenericInstantiator();
  this->functionValueChecker = &all.getFunctionValueChecker();
  this->objectLiteralChecker = &all.getObjectLiteralChecker();
}

Expr *ExprChecker::checkExpr(Expr *expr, const Type *expected, bool wraps) {
  GenericStore &generics = this->state.getGenericStore();
  if (generics.isCheckingInstance() && generics.hasForcedTypes()) {
    if (const Type *to = generics.findForcedType(expr)) {
      return this->genericChecker->forceConversion(this->checkExprKind(expr, expected, wraps), to);
    }
  }

  return this->checkExprKind(expr, expected, wraps);
}

Expr *ExprChecker::checkExprKind(Expr *expr, const Type *expected, bool wraps) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (FuncSymbol *generic = this->genericChecker->genericNamed(name)) {
      const Type *function = expected ? expected->nonOptional() : nullptr;
      Signature sig =
          function && function->isFunction() ? ClosureChecker::signatureOf(function) : Signature();
      return this->genericInstantiator->checkGenericValue(
          name, generic, function && function->isFunction() ? &sig : nullptr);
    }
  }
  if (expected && (isa<NameExpr>(expr) || isa<MemberExpr>(expr))) {
    if (const Type *function = expected->nonOptional(); function->isFunction()) {
      Signature sig = ClosureChecker::signatureOf(function);
      if (FunctionExpr *wrapper = this->functionValueChecker->builtinFunctionValue(
              expr, sig, unsigned(sig.params.size()), {})) {
        return this->closureChecker->checkFunctionExpr(wrapper, &sig);
      }
    }
  }
  switch (expr->kind()) {
    case Expr::Kind::IntLit: {
      auto *lit = cast<IntLitExpr>(expr);
      LiteralChecker::ResultIntLiteralTyped typed =
          this->literalChecker->checkIntLit(lit, expected, false);
      if (typed.isErr()) {
        this->state.getDiagnostics().error(lit->loc(), typed.error().message);
        return lit;
      }
      return typed.value();
    }
  case Expr::Kind::FloatLit: {
    if (const Type *literal =
            this->literalTypes->numberLiteralFor(expected, cast<FloatLitExpr>(expr)->value, true)) {
      expr->type = literal;

      return expr;
    }
    const Type *member = LiteralTypes::numberMemberOf(expected, true);
    if (expected && (expected->nonOptional()->isDecimal() || (member && member->isDecimal()))) {
      expr->type = Type::decimalTy();
      this->decimalChecker->checkDecimalLiteralRange(expr);
      return expr;
    }
    expr->type = Type::doubleTy();
    return expr;
  }
  case Expr::Kind::BoolLit: {
    const Type *literal =
        this->literalTypes->boolLiteralFor(expected, cast<BoolLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::boolTy();
    return expr;
  }
  case Expr::Kind::StringLit: {
    const Type *literal =
        this->literalTypes->stringLiteralFor(expected, cast<StringLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::string();
    return expr;
  }
  case Expr::Kind::RegexLit:
    return this->regexChecker->checkRegexLit(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    expr->type = Type::undefinedTy();
    return expr;
  case Expr::Kind::Template:
    return this->literalChecker->checkTemplate(cast<TemplateExpr>(expr), expected);
  case Expr::Kind::ArrayLit:
    return this->arrayLiteralChecker->checkArrayLit(cast<ArrayLitExpr>(expr), expected);
  case Expr::Kind::ObjectLit:
    return this->objectLiteralChecker->checkObjectLit(cast<ObjectLitExpr>(expr), expected);
  case Expr::Kind::Name:
    if (Expr *null = this->jsonChecker->checkNull(cast<NameExpr>(expr), expected)) {
      return null;
    }
    if (Expr *members = this->objectChecker->enumObject(cast<NameExpr>(expr), expected)) {
      return members;
    }
    return this->literalTypes->constText(this->nameChecker->checkName(cast<NameExpr>(expr)),
                                         expected);
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    MemberChecker::ResultMemberReadable read = this->memberChecker->checkMember(member, expected);
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
    MemberChecker::ResultIndexedElement element = this->memberChecker->checkIndex(index);
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
    return this->unaryChecker->checkUnary(cast<UnaryExpr>(expr), expected, wraps);
  case Expr::Kind::Binary:
    return this->binaryChecker->checkBinary(cast<BinaryExpr>(expr), expected, wraps);
  case Expr::Kind::Assign:
    return this->assignChecker->checkAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return this->assignChecker->checkUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return this->callChecker->checkCall(cast<CallExpr>(expr), expected);
  case Expr::Kind::New:
    return this->newChecker->checkNew(cast<NewExpr>(expr), expected);
  case Expr::Kind::Class:
    return this->lateClasses->checkClassExpr(cast<ClassExpr>(expr), expected);
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
      return this->closureChecker->checkFunctionExpr(cast<FunctionExpr>(expr), &sig);
    }
    return this->closureChecker->checkFunctionExpr(cast<FunctionExpr>(expr), nullptr);
  }
  case Expr::Kind::Yield:
    return this->generatorChecker->checkYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return this->asyncChecker->checkAwait(cast<AwaitExpr>(expr), expected);
  }
  llvm_unreachable("unknown expression kind");
}

Expr *ExprChecker::checkSequence(SequenceExpr *seq, const Type *expected, bool wraps) {
  for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
    seq->exprs[i] = this->checkExpr(seq->exprs[i]);
    ResultChecker::ResultValueGetsUsed used = this->resultChecker->usedValue(seq->exprs[i]);
    if (used.isErr()) {
      this->state.getDiagnostics().error(seq->exprs[i]->loc(), used.error().message);
    }
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
  cond->cond = this->truthChecker->checkCondition(cond->cond, whenTrue, whenFalse);
  if (std::optional<bool> constant = this->state.getGenericStore().isCheckingInstance()
                                         ? this->genericChecker->constantCondition(cond->cond)
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
  if (a != b && this->typeRelations->literalAdapts(cond->thenExpr, b)) {
    cond->thenExpr = this->typeRelations->adaptLiteral(cond->thenExpr, b);
    a = b;
  } else if (a != b && this->typeRelations->literalAdapts(cond->elseExpr, a)) {
    cond->elseExpr = this->typeRelations->adaptLiteral(cond->elseExpr, a);
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
  cond->thenExpr = this->typeRelations->convert(cond->thenExpr, common);
  cond->elseExpr = this->typeRelations->convert(cond->elseExpr, common);
  cond->type = common;

  return ok(cond);
}

ExprChecker::ResultCastSupported ExprChecker::checkCast(CastExpr *cast) {
  const Type *to = this->typeResolver->resolveType(*cast->target, false);
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
