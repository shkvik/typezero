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
  this->flowChecker = &all.flowChecker;
  this->destination = &all.destination;
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

Expr *ExprChecker::checkExpr(Expr *expr, const Type *expected) {
  auto &generics = this->state.getGenericStore();
  bool isForcing = generics.isCheckingInstance() && generics.hasForcedTypes();
  const auto *to = isForcing ? generics.findForcedType(expr) : nullptr;
  auto *checked = this->checkExprKind(expr, expected);
  if (to == nullptr) {
    return checked;
  }

  return this->genericChecker->forceConversion(checked, to);
}

Expr *ExprChecker::checkExprKind(Expr *expr, const Type *expected) {
  auto *value = this->checkFunctionValue(expr, expected);
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
      return this->unaryChecker->checkUnary(cast<UnaryExpr>(expr), expected);
    case Expr::Kind::Binary:
      return this->binaryChecker->checkBinary(cast<BinaryExpr>(expr), expected);
    case Expr::Kind::Assign:
      return this->assignChecker->checkAssign(cast<AssignExpr>(expr));
    case Expr::Kind::Update:
      return this->assignChecker->checkUpdate(cast<UpdateExpr>(expr));
    case Expr::Kind::Call:
      return this->callChecker->checkCall(cast<CallExpr>(expr), expected);
    case Expr::Kind::New:
      return this->newChecker->checkNew(cast<NewExpr>(expr), expected);
    case Expr::Kind::Class:
      return this->lateClasses->checkClassExpr(cast<ClassExpr>(expr));
    case Expr::Kind::Conditional:
      return this->checkConditionalValue(cast<ConditionalExpr>(expr), expected);
    case Expr::Kind::Cast:
      return this->checkCastValue(cast<CastExpr>(expr));
    case Expr::Kind::Sequence:
      return this->checkSequence(cast<SequenceExpr>(expr), expected);
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

std::optional<Signature> ExprChecker::expectedSignature(const Type *expected) const {
  const Type *function = expected != nullptr ? expected->nonOptional() : nullptr;
  if (function == nullptr || !function->isFunction()) {
    return std::nullopt;
  }

  return this->closureChecker->signatureOf(function);
}

Expr *ExprChecker::checkFunctionValue(Expr *expr, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(expr);
  FuncSymbol *generic = name != nullptr ? this->genericChecker->genericNamed(name) : nullptr;
  if (generic != nullptr) {
    auto signature = this->expectedSignature(expected);
    const Signature *wanted = signature.has_value() ? &*signature : nullptr;

    return this->genericInstantiator->checkGenericValue(name, generic, wanted);
  }
  bool isNamed = isa<NameExpr>(expr) || isa<MemberExpr>(expr);
  std::optional<Signature> signature = isNamed ? this->expectedSignature(expected) : std::nullopt;
  if (!signature.has_value()) {
    return nullptr;
  }
  auto *wrapper = this->functionValueChecker->builtinFunctionValue(
      expr, *signature, unsigned(signature->params.size()), {});
  if (wrapper == nullptr) {
    return nullptr;
  }

  return this->closureChecker->checkFunctionExpr(wrapper, &*signature);
}

Expr *ExprChecker::checkIntLiteral(IntLitExpr *literal, const Type *expected) {
  auto typed = this->literalChecker->checkIntLit(literal, expected);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), typed.error().message);
    return literal;
  }

  return typed.value();
}

Expr *ExprChecker::checkFloatLiteral(FloatLitExpr *literalExpr, const Type *expected) {
  const auto *literal =
      this->literalTypes->numberLiteralFor(expected, literalExpr->value, NumberSpelling::FRACTION);
  if (literal != nullptr) {
    literalExpr->type = literal;
    return literalExpr;
  }
  const auto *member = this->literalTypes->numberMemberOf(expected, NumberSpelling::FRACTION);
  bool isDecimal = expected != nullptr && (expected->nonOptional()->isDecimal() ||
                                           (member != nullptr && member->isDecimal()));
  if (isDecimal) {
    literalExpr->type = Type::decimalTy();
    auto range = this->decimalChecker->checkDecimalLiteralRange(literalExpr);
    if (range.isErr()) {
      this->state.getDiagnostics().error(literalExpr->loc(), range.error().message);
    }
    return literalExpr;
  }
  literalExpr->type = Type::doubleTy();

  return literalExpr;
}

Expr *ExprChecker::checkBoolLiteral(BoolLitExpr *literalExpr, const Type *expected) {
  const auto *literal = this->literalTypes->boolLiteralFor(expected, literalExpr->value);
  literalExpr->type = literal != nullptr ? literal : Type::boolTy();

  return literalExpr;
}

Expr *ExprChecker::checkStringLiteral(StringLitExpr *literalExpr, const Type *expected) {
  const auto *literal = this->literalTypes->stringLiteralFor(expected, literalExpr->value);
  literalExpr->type = literal != nullptr ? literal : Type::string();

  return literalExpr;
}

Expr *ExprChecker::checkNameValue(NameExpr *name, const Type *expected) {
  auto *null = this->jsonChecker->checkNull(name, expected);
  if (null != nullptr) {
    return null;
  }
  auto *members = this->objectChecker->enumObject(name, expected);
  if (members != nullptr) {
    return members;
  }
  auto *checked = this->nameChecker->checkName(name);

  return this->literalTypes->constText(checked, expected);
}

Expr *ExprChecker::checkMemberValue(MemberExpr *member, const Type *expected) {
  auto read = this->memberChecker->checkMember(member, expected);
  if (read.isOk()) {
    return read.value();
  }
  bool isOnObject = read.error().code == MemberCheckerCheckMemberErrorCode::MAY_BE_UNDEFINED ||
                    read.error().code == MemberCheckerCheckMemberErrorCode::UNKNOWN_NOT_NARROWED;
  llvm::SMLoc at = isOnObject ? member->object->loc() : member->loc();
  this->state.getDiagnostics().error(at, read.error().message);

  return member;
}

Expr *ExprChecker::checkIndexValue(IndexExpr *index) {
  auto element = this->memberChecker->checkIndex(index);
  if (element.isOk()) {
    return element.value();
  }
  if (element.error().code == MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED) {
    return index;
  }
  auto at = this->indexErrorLoc(index, element.error().code);
  this->state.getDiagnostics().error(at, element.error().message);

  return index;
}

llvm::SMLoc ExprChecker::indexErrorLoc(const IndexExpr *index,
                                       MemberCheckerCheckIndexErrorCode code) const {
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
  auto matched = this->checkConditional(conditional, expected);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(conditional->loc(), matched.error().message);
    return conditional;
  }

  return matched.value();
}

Expr *ExprChecker::checkCastValue(CastExpr *castExpr) {
  auto supported = this->checkCast(castExpr);
  if (supported.isErr()) {
    this->state.getDiagnostics().error(castExpr->loc(), supported.error().message);
    return castExpr;
  }

  return supported.value();
}

Expr *ExprChecker::checkFunctionLiteral(FunctionExpr *functionExpr, const Type *expected) {
  auto signature = this->expectedSignature(expected);
  const Signature *wanted = signature.has_value() ? &*signature : nullptr;

  return this->closureChecker->checkFunctionExpr(functionExpr, wanted);
}

Expr *ExprChecker::checkSequence(SequenceExpr *sequence, const Type *expected) {
  for (size_t index = 0; index + 1 < sequence->exprs.size(); ++index) {
    sequence->exprs[index] = this->checkExpr(sequence->exprs[index]);
    auto used = this->resultChecker->usedValue(sequence->exprs[index]);
    if (used.isErr()) {
      this->state.getDiagnostics().error(sequence->exprs[index]->loc(), used.error().message);
    }
  }
  Expr *&last = sequence->exprs.back();
  last = this->checkExpr(last, expected);
  sequence->type = last->type;

  return sequence;
}

Expr *ExprChecker::checkOptionalChain(OptionalChainExpr *chain) {
  this->state.getFunctionStack().enterOptionalChain();
  auto before = this->state.getFlowState().getFlow();
  chain->chain = this->checkExpr(chain->chain);
  chain->mayShortCircuit = this->state.getFunctionStack().isShortCircuiting();
  this->state.getFunctionStack().leaveOptionalChain();
  const Type *type = chain->chain->type;
  chain->type = type;
  if (!chain->mayShortCircuit || type->isError()) {
    return chain;
  }
  const auto &after = this->state.getFlowState().getFlow();
  auto joined = this->flowChecker->join(before, after);
  this->state.getFlowState().replaceFlow(joined);
  if (!type->isVoid()) {
    chain->type = Type::optional(type);
  }

  return chain;
}

Expr *ExprChecker::chainLink(Expr *object) {
  const Type *type = object->type;
  if (type->nonOptional()->isJson()) {
    return this->shortCircuitLink(object, Type::json());
  }
  if (!type->isOptional()) {
    return object;
  }

  return this->shortCircuitLink(object, type->inner());
}

Expr *ExprChecker::shortCircuitLink(Expr *object, const Type *type) {
  auto *link = this->state.getAst().make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type;
  link->shortCircuit = true;
  this->state.getFunctionStack().markShortCircuit();

  return link;
}

ExprChecker::ResultBranchesMatch ExprChecker::checkConditional(ConditionalExpr *conditional,
                                                               const Type *expected) {
  auto flows = ConditionFlows();
  conditional->cond =
      this->truthChecker->checkCondition(conditional->cond, flows.whenTrue, flows.whenFalse);
  auto constant = this->genericChecker->instanceConstant(conditional->cond);
  if (constant.has_value()) {
    const Flow &taken = *constant ? flows.whenTrue : flows.whenFalse;
    Expr *branch = *constant ? conditional->thenExpr : conditional->elseExpr;
    this->state.getFlowState().replaceFlow(taken);
    auto *chosen = this->checkExpr(branch, expected);

    return ok(chosen);
  }
  bool isThenWaiting = this->isWaitingForOther(conditional->thenExpr);
  bool isElseWaiting = this->isWaitingForOther(conditional->elseExpr);
  if (isThenWaiting && !isElseWaiting) {
    this->walkElseFirst(conditional, expected, flows);
  } else {
    this->walkThenFirst(conditional, expected, flows, isElseWaiting && !isThenWaiting);
  }

  return this->joinBranches(conditional);
}

bool ExprChecker::isWaitingForOther(const Expr *branch) const {
  return this->destination->isLiteralShaped(branch) || this->decimalChecker->isLiteralTree(branch);
}

const Type *ExprChecker::branchHint(const Expr *literal, const Expr *other,
                                    const Type *expected) const {
  if (this->decimalChecker->isLiteralTree(literal)) {
    return this->decimalChecker->literalHint(other, expected);
  }
  if (expected != nullptr) {
    return expected;
  }

  return other->type->isError() ? nullptr : other->type;
}

void ExprChecker::walkElseFirst(ConditionalExpr *conditional, const Type *expected,
                                const ConditionFlows &flows) {
  this->state.getFlowState().replaceFlow(flows.whenFalse);
  conditional->elseExpr = this->checkExpr(conditional->elseExpr, expected);
  auto afterElse = this->state.getFlowState().getFlow();
  this->state.getFlowState().replaceFlow(flows.whenTrue);
  const auto *hint = this->branchHint(conditional->thenExpr, conditional->elseExpr, expected);
  conditional->thenExpr = this->checkExpr(conditional->thenExpr, hint);
  const auto &afterThen = this->state.getFlowState().getFlow();
  auto joined = this->flowChecker->join(afterThen, afterElse);
  this->state.getFlowState().replaceFlow(joined);
}

void ExprChecker::walkThenFirst(ConditionalExpr *conditional, const Type *expected,
                                const ConditionFlows &flows, bool isElseWaitingOnly) {
  this->state.getFlowState().replaceFlow(flows.whenTrue);
  conditional->thenExpr = this->checkExpr(conditional->thenExpr, expected);
  auto afterThen = this->state.getFlowState().getFlow();
  this->state.getFlowState().replaceFlow(flows.whenFalse);
  const auto *elseHint = this->elseHint(conditional, expected, isElseWaitingOnly);
  conditional->elseExpr = this->checkExpr(conditional->elseExpr, elseHint);
  const auto &afterElse = this->state.getFlowState().getFlow();
  auto joined = this->flowChecker->join(afterThen, afterElse);
  this->state.getFlowState().replaceFlow(joined);
}

const Type *ExprChecker::elseHint(const ConditionalExpr *conditional, const Type *expected,
                                  bool isElseWaitingOnly) const {
  if (isElseWaitingOnly) {
    return this->branchHint(conditional->elseExpr, conditional->thenExpr, expected);
  }
  if (this->decimalChecker->isLiteralTree(conditional->elseExpr)) {
    return this->decimalChecker->literalHint(conditional->thenExpr, expected);
  }

  return expected;
}

ExprChecker::ResultBranchesMatch ExprChecker::joinBranches(ConditionalExpr *conditional) {
  const Type *thenType = conditional->thenExpr->type;
  const Type *elseType = conditional->elseExpr->type;
  conditional->type = Type::error();
  if (thenType->isError() || elseType->isError()) {
    conditional->type = this->failedBranchesType(thenType, elseType);
    return ok(conditional);
  }
  if (thenType->isVoid() && elseType->isVoid()) {
    conditional->type = thenType;
    return ok(conditional);
  }
  bool isDifferent = thenType != elseType;
  bool isThenAdapting =
      isDifferent && this->typeRelations->isLiteralAdapting(conditional->thenExpr, elseType);
  bool isElseAdapting = isDifferent && !isThenAdapting &&
                        this->typeRelations->isLiteralAdapting(conditional->elseExpr, thenType);
  if (isThenAdapting) {
    conditional->thenExpr = this->typeRelations->adaptLiteral(conditional->thenExpr, elseType);
  }
  if (isElseAdapting) {
    conditional->elseExpr = this->typeRelations->adaptLiteral(conditional->elseExpr, thenType);
  }
  const Type *left = isThenAdapting ? elseType : thenType;
  const Type *right = isElseAdapting ? thenType : elseType;
  const auto *common = LiteralTypes::unionUnify(left, right);
  if (common == nullptr) {
    auto message = ErrorTextConditional::mismatch(left->name(), right->name());

    return err(ExprCheckerCheckConditionalError{
        .code = ExprCheckerCheckConditionalErrorCode::INCOMPATIBLE_BRANCHES,
        .message = message,
    });
  }
  conditional->thenExpr = this->typeRelations->convert(conditional->thenExpr, common);
  conditional->elseExpr = this->typeRelations->convert(conditional->elseExpr, common);
  conditional->type = common;

  return ok(conditional);
}

const Type *ExprChecker::failedBranchesType(const Type *thenType, const Type *elseType) const {
  bool isOneFailed = thenType->isError() != elseType->isError();
  if (!this->state.getFunctionStack().isInsideTrial() || !isOneFailed) {
    return Type::error();
  }

  return thenType->isError() ? elseType : thenType;
}

ExprChecker::ResultCastSupported ExprChecker::checkCast(CastExpr *castExpr) {
  const auto *to = this->typeResolver->resolveType(*castExpr->target);
  bool isArrayLiteral = isa<ArrayLitExpr>(castExpr->operand);
  const Type *hint = isArrayLiteral && !to->isError() ? to : nullptr;
  castExpr->operand = this->checkExpr(castExpr->operand, hint);
  castExpr->type = to;
  auto code = this->castProblem(castExpr->operand, to, isArrayLiteral);
  auto message = this->castMessage(code, castExpr->operand, to);

  return err(ExprCheckerCheckCastError{
      .code = code,
      .message = message,
  });
}

ExprCheckerCheckCastErrorCode ExprChecker::castProblem(const Expr *operand, const Type *to,
                                                       bool isArrayLiteral) const {
  const Type *from = operand->type;
  if (from->isError() || to->isError()) {
    return ExprCheckerCheckCastErrorCode::NOT_SUPPORTED;
  }
  if (isArrayLiteral) {
    return ExprCheckerCheckCastErrorCode::ARRAY_LITERAL;
  }
  if (from == to || TypeRelations::isImplicitlyConvertible(from, to)) {
    return ExprCheckerCheckCastErrorCode::CONVERTS_BY_ITSELF;
  }
  if (from->isOptional() && TypeRelations::isImplicitlyConvertible(from->inner(), to)) {
    return ExprCheckerCheckCastErrorCode::MAY_BE_UNDEFINED;
  }
  auto hint = this->typeRelations->conversionHint(operand, to);
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
                                     const Type *to) const {
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
      return this->hintedCastMessage(operand, to);
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

std::string ExprChecker::hintedCastMessage(const Expr *operand, const Type *to) const {
  auto hint = this->typeRelations->conversionHint(operand, to);

  return ErrorTextCast::hinted(hint);
}

}
