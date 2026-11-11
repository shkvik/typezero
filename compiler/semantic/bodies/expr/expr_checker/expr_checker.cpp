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
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ExprChecker::ExprChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->newChecker = &all.newChecker;
  this->callChecker = &all.callChecker;
  this->jsonChecker = &all.jsonChecker;
  this->lateClasses = &all.lateClasses;
  this->nameChecker = &all.nameChecker;
  this->asyncChecker = &all.asyncChecker;
  this->literalTypes = &all.literalTypes;
  this->regexChecker = &all.regexChecker;
  this->truthChecker = &all.truthChecker;
  this->typeResolver = &all.typeResolver;
  this->unaryChecker = &all.unaryChecker;
  this->assignChecker = &all.assignChecker;
  this->binaryChecker = &all.binaryChecker;
  this->memberChecker = &all.memberChecker;
  this->objectChecker = &all.objectChecker;
  this->resultChecker = &all.resultChecker;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->decimalChecker = &all.decimalChecker;
  this->genericChecker = &all.genericChecker;
  this->literalChecker = &all.literalChecker;
  this->generatorChecker = &all.generatorChecker;
  this->arrayLiteralChecker = &all.arrayLiteralChecker;
  this->genericInstantiator = &all.genericInstantiator;
  this->functionValueChecker = &all.functionValueChecker;
  this->objectLiteralChecker = &all.objectLiteralChecker;
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
  Expr *value = this->checkFunctionValue(expr, expected);
  if (value != nullptr) {
    return value;
  }
  switch (expr->kind()) {
    case Expr::Kind::IntLit:
      return this->checkIntLiteral(cast<IntLitExpr>(expr), expected);
    case Expr::Kind::FloatLit:
      return this->checkFloatLiteral(cast<FloatLitExpr>(expr), expected);
    case Expr::Kind::BoolLit:
      return this->checkBoolLiteral(cast<BoolLitExpr>(expr), expected);
    case Expr::Kind::StringLit:
      return this->checkStringLiteral(cast<StringLitExpr>(expr), expected);
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
      return this->checkNameValue(cast<NameExpr>(expr), expected);
    case Expr::Kind::Member:
      return this->checkMemberValue(cast<MemberExpr>(expr), expected);
    case Expr::Kind::Index:
      return this->checkIndexValue(cast<IndexExpr>(expr));
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
    case Expr::Kind::Conditional:
      return this->checkConditionalValue(cast<ConditionalExpr>(expr), expected);
    case Expr::Kind::Cast:
      return this->checkCastValue(cast<CastExpr>(expr));
    case Expr::Kind::Sequence:
      return this->checkSequence(cast<SequenceExpr>(expr), expected, wraps);
    case Expr::Kind::OptionalChain:
      return this->checkOptionalChain(cast<OptionalChainExpr>(expr));
    case Expr::Kind::Function:
      return this->checkFunctionLiteral(cast<FunctionExpr>(expr), expected);
    case Expr::Kind::Yield:
      return this->generatorChecker->checkYield(cast<YieldExpr>(expr));
    case Expr::Kind::Await:
      return this->asyncChecker->checkAwait(cast<AwaitExpr>(expr), expected);
  }

  return expr;
}

std::optional<Signature> ExprChecker::expectedSignature(const Type *expected) {
  const Type *function = expected != nullptr ? expected->nonOptional() : nullptr;
  if (function == nullptr || !function->isFunction()) {
    return std::nullopt;
  }

  return ClosureChecker::signatureOf(function);
}

Expr *ExprChecker::checkFunctionValue(Expr *expr, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(expr);
  FuncSymbol *generic = name != nullptr ? this->genericChecker->genericNamed(name) : nullptr;
  if (generic != nullptr) {
    std::optional<Signature> sig = ExprChecker::expectedSignature(expected);
    return this->genericInstantiator->checkGenericValue(name, generic,
                                                        sig.has_value() ? &*sig : nullptr);
  }
  bool named = isa<NameExpr>(expr) || isa<MemberExpr>(expr);
  std::optional<Signature> sig = named ? ExprChecker::expectedSignature(expected) : std::nullopt;
  if (!sig.has_value()) {
    return nullptr;
  }
  FunctionExpr *wrapper = this->functionValueChecker->builtinFunctionValue(
      expr, *sig, unsigned(sig->params.size()), {});
  if (wrapper == nullptr) {
    return nullptr;
  }

  return this->closureChecker->checkFunctionExpr(wrapper, &*sig);
}

Expr *ExprChecker::checkIntLiteral(IntLitExpr *lit, const Type *expected) {
  LiteralChecker::ResultIntLiteralTyped typed =
      this->literalChecker->checkIntLit(lit, expected, false);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), typed.error().message);
    return lit;
  }

  return typed.value();
}

Expr *ExprChecker::checkFloatLiteral(FloatLitExpr *lit, const Type *expected) {
  const Type *literal = this->literalTypes->numberLiteralFor(expected, lit->value, true);
  if (literal != nullptr) {
    lit->type = literal;
    return lit;
  }
  const Type *member = LiteralTypes::numberMemberOf(expected, true);
  bool decimal = expected != nullptr && (expected->nonOptional()->isDecimal() ||
                                         (member != nullptr && member->isDecimal()));
  if (decimal) {
    lit->type = Type::decimalTy();
    this->decimalChecker->checkDecimalLiteralRange(lit);
    return lit;
  }
  lit->type = Type::doubleTy();

  return lit;
}

Expr *ExprChecker::checkBoolLiteral(BoolLitExpr *lit, const Type *expected) {
  const Type *literal = this->literalTypes->boolLiteralFor(expected, lit->value);
  lit->type = literal != nullptr ? literal : Type::boolTy();

  return lit;
}

Expr *ExprChecker::checkStringLiteral(StringLitExpr *lit, const Type *expected) {
  const Type *literal = this->literalTypes->stringLiteralFor(expected, lit->value);
  lit->type = literal != nullptr ? literal : Type::string();

  return lit;
}

Expr *ExprChecker::checkNameValue(NameExpr *name, const Type *expected) {
  Expr *null = this->jsonChecker->checkNull(name, expected);
  if (null != nullptr) {
    return null;
  }
  Expr *members = this->objectChecker->enumObject(name, expected);
  if (members != nullptr) {
    return members;
  }
  Expr *checked = this->nameChecker->checkName(name);

  return this->literalTypes->constText(checked, expected);
}

Expr *ExprChecker::checkMemberValue(MemberExpr *member, const Type *expected) {
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

Expr *ExprChecker::checkIndexValue(IndexExpr *index) {
  MemberChecker::ResultIndexedElement element = this->memberChecker->checkIndex(index);
  if (element.isOk()) {
    return element.value();
  }
  if (element.error().code == MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED) {
    return index;
  }
  llvm::SMLoc at = ExprChecker::indexErrorLoc(index, element.error().code);
  this->state.getDiagnostics().error(at, element.error().message);

  return index;
}

llvm::SMLoc ExprChecker::indexErrorLoc(const IndexExpr *index,
                                       MemberCheckerCheckIndexErrorCode code) {
  switch (code) {
    case MemberCheckerCheckIndexErrorCode::ENUM_NOT_INTEGER:
    case MemberCheckerCheckIndexErrorCode::GENERIC_INDEX:
    case MemberCheckerCheckIndexErrorCode::KEY_REJECTED:
      return index->index->loc();
    case MemberCheckerCheckIndexErrorCode::UNKNOWN_NOT_NARROWED:
    case MemberCheckerCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED:
      return index->object->loc();
    case MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED:
    case MemberCheckerCheckIndexErrorCode::ENUM_STRING:
    case MemberCheckerCheckIndexErrorCode::FIELD_REJECTED:
    case MemberCheckerCheckIndexErrorCode::DICT_INDEX:
    case MemberCheckerCheckIndexErrorCode::NOT_INDEXABLE:
      return index->loc();
  }

  return index->loc();
}

Expr *ExprChecker::checkConditionalValue(ConditionalExpr *conditional, const Type *expected) {
  ResultBranchesMatch matched = this->checkConditional(conditional, expected);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(conditional->loc(), matched.error().message);
    return conditional;
  }

  return matched.value();
}

Expr *ExprChecker::checkCastValue(CastExpr *castExpr) {
  ResultCastSupported supported = this->checkCast(castExpr);
  if (supported.isErr()) {
    this->state.getDiagnostics().error(castExpr->loc(), supported.error().message);
    return castExpr;
  }

  return supported.value();
}

Expr *ExprChecker::checkFunctionLiteral(FunctionExpr *fn, const Type *expected) {
  std::optional<Signature> sig = ExprChecker::expectedSignature(expected);

  return this->closureChecker->checkFunctionExpr(fn, sig.has_value() ? &*sig : nullptr);
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
  ConditionFlows flows;
  cond->cond = this->truthChecker->checkCondition(cond->cond, flows.whenTrue, flows.whenFalse);
  std::optional<bool> constant = this->state.getGenericStore().isCheckingInstance()
                                     ? this->genericChecker->constantCondition(cond->cond)
                                     : std::nullopt;
  if (constant.has_value()) {
    this->state.getFlowState().replaceFlow(*constant ? flows.whenTrue : flows.whenFalse);
    Expr *chosen = this->checkExpr(*constant ? cond->thenExpr : cond->elseExpr, expected);
    return ok(chosen);
  }
  bool thenWaits = ExprChecker::waitsForOther(cond->thenExpr);
  bool elseWaits = ExprChecker::waitsForOther(cond->elseExpr);
  if (thenWaits && !elseWaits) {
    this->walkElseFirst(cond, expected, flows);
  } else {
    this->walkThenFirst(cond, expected, flows, elseWaits && !thenWaits);
  }

  return this->joinBranches(cond);
}

bool ExprChecker::waitsForOther(const Expr *branch) {
  return Destination::literalShaped(branch) || DecimalChecker::isLiteralTree(branch);
}

const Type *ExprChecker::branchHint(const Expr *literal, const Expr *other, const Type *expected) {
  if (DecimalChecker::isLiteralTree(literal)) {
    return DecimalChecker::literalHint(other, expected);
  }
  if (expected != nullptr) {
    return expected;
  }

  return other->type->isError() ? nullptr : other->type;
}

void ExprChecker::walkElseFirst(ConditionalExpr *cond, const Type *expected,
                                const ConditionFlows &flows) {
  this->state.getFlowState().replaceFlow(flows.whenFalse);
  cond->elseExpr = this->checkExpr(cond->elseExpr, expected);
  Flow afterElse = this->state.getFlowState().getFlow();
  this->state.getFlowState().replaceFlow(flows.whenTrue);
  const Type *hint = ExprChecker::branchHint(cond->thenExpr, cond->elseExpr, expected);
  cond->thenExpr = this->checkExpr(cond->thenExpr, hint);
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(this->state.getFlowState().getFlow(), afterElse));
}

void ExprChecker::walkThenFirst(ConditionalExpr *cond, const Type *expected,
                                const ConditionFlows &flows, bool elseWaitsOnly) {
  this->state.getFlowState().replaceFlow(flows.whenTrue);
  cond->thenExpr = this->checkExpr(cond->thenExpr, expected);
  Flow afterThen = this->state.getFlowState().getFlow();
  this->state.getFlowState().replaceFlow(flows.whenFalse);
  const Type *elseHint = expected;
  if (elseWaitsOnly) {
    elseHint = ExprChecker::branchHint(cond->elseExpr, cond->thenExpr, expected);
  } else if (DecimalChecker::isLiteralTree(cond->elseExpr)) {
    elseHint = DecimalChecker::literalHint(cond->thenExpr, expected);
  }
  cond->elseExpr = this->checkExpr(cond->elseExpr, elseHint);
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(afterThen, this->state.getFlowState().getFlow()));
}

ExprChecker::ResultBranchesMatch ExprChecker::joinBranches(ConditionalExpr *cond) {
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
  if (common == nullptr) {
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
  cast->type = to;
  ExprCheckerCheckCastErrorCode code = ExprChecker::castProblem(cast->operand, to, arrayLiteral);
  std::string message = ExprChecker::castMessage(code, cast->operand, to);

  return err(ExprCheckerCheckCastError{
      .code = code,
      .message = message,
  });
}

ExprCheckerCheckCastErrorCode ExprChecker::castProblem(const Expr *operand, const Type *to,
                                                       bool arrayLiteral) {
  const Type *from = operand->type;
  if (from->isError() || to->isError()) {
    return ExprCheckerCheckCastErrorCode::NOT_SUPPORTED;
  }
  if (arrayLiteral) {
    return ExprCheckerCheckCastErrorCode::ARRAY_LITERAL;
  }
  if (from == to || TypeRelations::isImplicitlyConvertible(from, to)) {
    return ExprCheckerCheckCastErrorCode::CONVERTS_BY_ITSELF;
  }
  if (from->isOptional() && TypeRelations::isImplicitlyConvertible(from->inner(), to)) {
    return ExprCheckerCheckCastErrorCode::MAY_BE_UNDEFINED;
  }
  std::string hint = TypeRelations::conversionHint(operand, to);
  if (!hint.empty()) {
    return ExprCheckerCheckCastErrorCode::NEEDS_CONVERSION;
  }
  if (from->isFloat() && to->isDecimal()) {
    return ExprCheckerCheckCastErrorCode::DOUBLE_TO_DECIMAL;
  }
  if (from->isDecimal() && to->isNumeric()) {
    return ExprCheckerCheckCastErrorCode::DECIMAL_TO_NUMBER;
  }
  if (from->nonOptional()->isJson()) {
    return ExprCheckerCheckCastErrorCode::JSON_VALUE;
  }

  return ExprCheckerCheckCastErrorCode::CANNOT_BECOME;
}

std::string ExprChecker::castMessage(ExprCheckerCheckCastErrorCode code, const Expr *operand,
                                     const Type *to) {
  const Type *from = operand->type;
  switch (code) {
    case ExprCheckerCheckCastErrorCode::NOT_SUPPORTED:
      return ErrorTextCast::plain();
    case ExprCheckerCheckCastErrorCode::ARRAY_LITERAL:
      return ErrorTextCast::declare(to->name());
    case ExprCheckerCheckCastErrorCode::CONVERTS_BY_ITSELF:
      return ErrorTextCast::itself(to->name());
    case ExprCheckerCheckCastErrorCode::MAY_BE_UNDEFINED:
      return ErrorTextCast::undefined();
    case ExprCheckerCheckCastErrorCode::NEEDS_CONVERSION:
      return ExprChecker::hintedCastMessage(operand, to);
    case ExprCheckerCheckCastErrorCode::DOUBLE_TO_DECIMAL:
      return ErrorTextCast::toDecimal();
    case ExprCheckerCheckCastErrorCode::DECIMAL_TO_NUMBER:
      return ErrorTextCast::toNumber();
    case ExprCheckerCheckCastErrorCode::JSON_VALUE:
      return ErrorTextCast::json();
    case ExprCheckerCheckCastErrorCode::CANNOT_BECOME:
      return ErrorTextCast::cannot(from->name(), to->name());
  }

  return ErrorTextCast::cannot(from->name(), to->name());
}

std::string ExprChecker::hintedCastMessage(const Expr *operand, const Type *to) {
  std::string hint = TypeRelations::conversionHint(operand, to);

  return ErrorTextCast::hinted(hint);
}

}
