#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/StringExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";
constexpr const char *kEllipsis = "...";
constexpr const char *kArgumentSeparator = ", ";
constexpr const char *kAssignSuffix = "=";
constexpr char16_t kFirstPrintable = 0x20;
constexpr char16_t kFirstNonAscii = 0x7f;

}

TruthChecker::TruthChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->jsonChecker = &all.jsonChecker;
  this->flowChecker = &all.flowChecker;
  this->typeRelations = &all.typeRelations;
  this->conditionFacts = &all.conditionFacts;
  this->genericChecker = &all.genericChecker;
}

std::string TruthChecker::sourceText(const Expr *expr) const {
  auto answers = std::vector<std::string>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &TruthChecker::sourceParts)) {
    auto text = this->nodeSource(node, answers);
    if (text.empty()) {
      return "";
    }
    answers.push_back(std::move(text));
  }

  return answers.back();
}

NodeChildren<const Expr *> TruthChecker::sourceParts(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *chain = dyn_cast<OptionalChainExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    return {conversion->operand};
  }
  if (chain != nullptr) {
    return {chain->chain};
  }
  if (member != nullptr) {
    return {member->object};
  }
  if (index != nullptr) {
    return {index->object, index->index};
  }
  if (call == nullptr || call->hasSpread()) {
    return {};
  }
  auto parts = NodeChildren<const Expr *>{call->callee};
  parts.append(call->args.begin(), call->args.end());

  return parts;
}

std::string TruthChecker::nodeSource(const Expr *expr, std::vector<std::string> &answers) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  auto *integer = dyn_cast<IntLitExpr>(expr);
  auto *boolean = dyn_cast<BoolLitExpr>(expr);
  auto *text = dyn_cast<StringLitExpr>(expr);
  auto *list = dyn_cast<ArrayLitExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);
  bool isPassing =
      (conversion != nullptr && conversion->isImplicit()) || isa<OptionalChainExpr>(expr);
  if (isPassing) {
    return ChildrenFirst::takeAnswer(answers);
  }
  if (name != nullptr) {
    return name->name.starts_with(kHiddenPrefix) ? "" : name->name.str();
  }
  if (integer != nullptr) {
    return std::to_string(integer->value);
  }
  if (boolean != nullptr) {
    return boolean->value ? "true" : "false";
  }
  if (isa<UndefinedExpr>(expr)) {
    return "undefined";
  }
  if (text != nullptr) {
    return this->stringSource(text);
  }
  if (list != nullptr) {
    return list->elements.empty() ? "[]" : "";
  }
  if (member != nullptr) {
    return this->memberSource(member, answers);
  }
  if (index != nullptr) {
    return this->indexSource(index, answers);
  }
  if (call != nullptr) {
    return this->callSource(call, answers);
  }

  return "";
}

std::string TruthChecker::memberSource(const MemberExpr *member,
                                       std::vector<std::string> &answers) const {
  auto object = ChildrenFirst::takeAnswer(answers);
  const char *dot = member->optional ? "?." : ".";

  return object + dot + member->name.str();
}

std::string TruthChecker::indexSource(const IndexExpr *index,
                                      std::vector<std::string> &answers) const {
  auto position = ChildrenFirst::takeAnswer(answers);
  auto object = ChildrenFirst::takeAnswer(answers);
  const char *open = index->optional ? "?.[" : "[";

  return object + open + position + "]";
}

std::string TruthChecker::stringSource(const StringLitExpr *literal) const {
  auto text = std::string("\"");
  for (char16_t unit : literal->value) {
    bool isPlain = unit >= kFirstPrintable && unit < kFirstNonAscii && unit != '"' && unit != '\\';
    if (!isPlain) {
      return "";
    }
    text += static_cast<char>(unit);
  }

  return text + "\"";
}

std::string TruthChecker::callSource(const CallExpr *call,
                                     std::vector<std::string> &answers) const {
  if (call->hasSpread()) {
    return "";
  }
  auto args = ChildrenFirst::takeAnswers(answers, call->args.size());
  auto callee = ChildrenFirst::takeAnswer(answers);
  const char *open = call->optional ? "?.(" : "(";
  auto joined = llvm::join(args, kArgumentSeparator);

  return callee + open + joined + ")";
}

std::string TruthChecker::hintText(const Expr *expr, const char *placeholder) const {
  std::string text = expr != nullptr ? this->sourceText(expr) : "";

  return text.empty() ? placeholder : text;
}

const NameExpr *TruthChecker::narrowedVariable(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion == nullptr) {
    return nullptr;
  }
  const Type *from = conversion->operand->type;
  bool isUnwrapping = conversion->isImplicit() && !conversion->shortCircuit && from->isOptional() &&
                      conversion->type == from->inner();
  if (!isUnwrapping) {
    return nullptr;
  }

  return dyn_cast<NameExpr>(conversion->operand);
}

bool TruthChecker::isLength(const Expr *expr) const {
  const auto *inner = this->withoutWrappers(expr);
  auto *member = dyn_cast<MemberExpr>(inner);

  return member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
}

const Expr *TruthChecker::withoutWrappers(const Expr *expr) const {
  const Expr *inner = expr;
  const auto *next = this->wrappedExpr(inner);
  while (next != nullptr) {
    inner = next;
    next = this->wrappedExpr(inner);
  }

  return inner;
}

const Expr *TruthChecker::wrappedExpr(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *chain = dyn_cast<OptionalChainExpr>(expr);
  if (conversion != nullptr) {
    return conversion->operand;
  }
  if (chain != nullptr) {
    return chain->chain;
  }

  return nullptr;
}

bool TruthChecker::isLiteral(const Expr *expr) const {
  const auto *bare = ExprParts::withoutCasts(expr);
  auto *unary = dyn_cast<UnaryExpr>(bare);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *value = isNegated ? unary->operand : bare;

  return isa<IntLitExpr>(value) || isa<FloatLitExpr>(value) || isa<StringLitExpr>(value) ||
         isa<BoolLitExpr>(value) || isa<UndefinedExpr>(value);
}

const Type *TruthChecker::falsyType(const Type *type) const {
  const Type *base = type->nonOptional();
  bool isAlwaysTruthy = base->isArray() || base->isFunction() || base->isObject() ||
                        base->isIterator() || base->isDict() || base->isClassRef();

  return isAlwaysTruthy ? Type::undefinedTy() : type;
}

Expr *TruthChecker::checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse) {
  return this->checkBranching(cond, whenTrue, whenFalse, TruthUse::Condition);
}

Expr *TruthChecker::checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use) {
  auto run = BranchRun();
  run.steps.push_back(BranchExpr{.expr = expr, .use = use});
  while (!run.steps.empty()) {
    BranchStep step = std::move(run.steps.back());
    run.steps.pop_back();
    this->applyBranchStep(step, run);
  }
  auto result = std::move(run.results.back());
  whenTrue = std::move(result.whenTrue);
  whenFalse = std::move(result.whenFalse);

  return result.expr;
}

void TruthChecker::applyBranchStep(BranchStep &step, BranchRun &run) {
  switch (step.kind()) {
    case BranchStepKind::EXPR:
      this->enterBranch(*step.as<BranchExpr>(), run);
      break;
    case BranchStepKind::LEFT_DONE:
      this->leftDone(step.as<LeftDone>()->binary, run);
      break;
    case BranchStepKind::RIGHT_DONE:
      this->rightDone(*step.as<RightDone>(), run);
      break;
    case BranchStepKind::NOT_DONE:
      this->notDone(step.as<NotDone>()->unary, run);
      break;
  }
}

void TruthChecker::enterBranch(const BranchExpr &branch, BranchRun &run) {
  Expr *expr = branch.expr;
  auto *binary = dyn_cast<BinaryExpr>(expr);
  auto *unary = dyn_cast<UnaryExpr>(expr);
  if (binary != nullptr && (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr)) {
    run.steps.push_back(LeftDone{.binary = binary});
    run.steps.push_back(BranchExpr{.expr = binary->lhs, .use = TruthUse::Condition});
    return;
  }
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    run.steps.push_back(NotDone{.unary = unary});
    run.steps.push_back(BranchExpr{.expr = unary->operand, .use = TruthUse::Not});
    return;
  }
  auto result = BranchResult();
  result.expr = this->branchLeaf(expr, result.whenTrue, result.whenFalse, branch.use);
  run.results.push_back(std::move(result));
}

void TruthChecker::leftDone(BinaryExpr *binary, BranchRun &run) {
  auto left = ChildrenFirst::takeAnswer(run.results);
  binary->lhs = left.expr;
  bool isAnd = binary->op == BinaryOp::LogAnd;
  auto constant = this->genericChecker->instanceConstant(binary->lhs);
  if (constant.has_value() && *constant != isAnd) {
    auto joined = this->flowChecker->join(left.whenTrue, left.whenFalse);
    this->state.getFlowState().replaceFlow(joined);
    run.results.push_back(std::move(left));
    return;
  }
  const Flow &rightStart = isAnd ? left.whenTrue : left.whenFalse;
  this->state.getFlowState().replaceFlow(rightStart);
  run.steps.push_back(RightDone{
      .binary = binary,
      .lhsTrue = std::move(left.whenTrue),
      .lhsFalse = std::move(left.whenFalse),
  });
  run.steps.push_back(BranchExpr{.expr = binary->rhs, .use = TruthUse::Condition});
}

void TruthChecker::rightDone(RightDone &done, BranchRun &run) {
  auto right = ChildrenFirst::takeAnswer(run.results);
  BinaryExpr *binary = done.binary;
  binary->rhs = right.expr;
  bool isAnd = binary->op == BinaryOp::LogAnd;
  auto whenTrue = isAnd ? right.whenTrue : this->flowChecker->join(done.lhsTrue, right.whenTrue);
  auto whenFalse =
      isAnd ? this->flowChecker->join(done.lhsFalse, right.whenFalse) : right.whenFalse;
  auto joined = this->flowChecker->join(whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(joined);
  this->jsonChecker->walkRedundantTests(binary);
  binary->type = Type::boolTy();
  run.results.push_back(BranchResult{
      .expr = binary,
      .whenTrue = std::move(whenTrue),
      .whenFalse = std::move(whenFalse),
  });
}

void TruthChecker::notDone(UnaryExpr *unary, BranchRun &run) const {
  auto operand = ChildrenFirst::takeAnswer(run.results);
  unary->operand = operand.expr;
  unary->type = Type::boolTy();
  run.results.push_back(BranchResult{
      .expr = unary,
      .whenTrue = std::move(operand.whenFalse),
      .whenFalse = std::move(operand.whenTrue),
  });
}

Expr *TruthChecker::branchLeaf(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use) {
  auto *assign = dyn_cast<AssignExpr>(expr);
  if (this->isBooleanCall(expr)) {
    return this->checkBooleanCall(cast<CallExpr>(expr), whenTrue, whenFalse);
  }
  if (assign != nullptr && !assign->op.has_value()) {
    return this->assignedLeaf(assign, whenTrue, whenFalse, use);
  }
  auto *checked = this->exprChecker->checkExpr(expr, Type::boolTy());
  this->addValueFacts(checked, whenTrue, whenFalse);
  if (!checked->type->isBool()) {
    auto truthy = this->checkTruthy(checked, use);
    if (truthy.isErr()) {
      this->state.getDiagnostics().error(checked->loc(), truthy.error().message);
    }
  }

  return this->typeRelations->convert(checked, Type::boolTy());
}

Expr *TruthChecker::assignedLeaf(AssignExpr *assign, Flow &whenTrue, Flow &whenFalse,
                                 TruthUse use) {
  auto *checked = this->exprChecker->checkExpr(assign);
  whenTrue = this->state.getFlowState().getFlow();
  whenFalse = this->state.getFlowState().getFlow();
  auto form = this->checkAssignmentCondition(assign, use);
  if (form.isErr()) {
    this->state.getDiagnostics().error(assign->loc(), form.error().message);
  }

  return this->typeRelations->convert(checked, Type::boolTy());
}

void TruthChecker::addValueFacts(Expr *value, Flow &whenTrue, Flow &whenFalse) {
  whenTrue = this->state.getFlowState().getFlow();
  whenFalse = this->state.getFlowState().getFlow();
  auto *literal = dyn_cast<BoolLitExpr>(value);
  if (literal == nullptr) {
    this->conditionFacts->addConditionFacts(value, whenTrue, whenFalse);
    return;
  }
  Flow &impossible = literal->value ? whenFalse : whenTrue;
  impossible.unreachable = true;
}

Expr *TruthChecker::checkTestedValue(Expr *expr, const Type *expected, Flow &whenTrue,
                                     Flow &whenFalse, TruthUse use) {
  auto *binary = dyn_cast<BinaryExpr>(expr);
  auto *unary = dyn_cast<UnaryExpr>(expr);
  if (binary != nullptr && (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr)) {
    auto logicalUse = binary->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or;
    return this->checkLogical(binary, expected, whenTrue, whenFalse, RightSide::TESTED, logicalUse);
  }
  bool isNot = unary != nullptr && unary->op == UnaryOp::Not;
  if (isNot || this->isBooleanCall(expr)) {
    return this->checkBranching(expr, whenTrue, whenFalse, use);
  }
  auto *checked = this->exprChecker->checkExpr(expr, expected);
  this->addValueFacts(checked, whenTrue, whenFalse);

  return checked;
}

Expr *TruthChecker::checkLogical(BinaryExpr *binary, const Type *expected, Flow &whenTrue,
                                 Flow &whenFalse, RightSide side, TruthUse use) {
  bool isAnd = binary->op == BinaryOp::LogAnd;
  bool isAssign = use == TruthUse::OrAssign || use == TruthUse::AndAssign;
  const Expr *other = isAssign ? cast<AssignExpr>(binary->rhs)->value : binary->rhs;
  auto lhsTrue = Flow();
  auto lhsFalse = Flow();
  auto rhsTrue = Flow();
  auto rhsFalse = Flow();
  auto *left = this->checkTestedValue(binary->lhs, expected, lhsTrue, lhsFalse, use);
  binary->lhs = this->genericChecker->apparent(left);
  auto leftTruthy = this->checkTruthy(binary->lhs, use, other);
  if (leftTruthy.isErr()) {
    this->state.getDiagnostics().error(binary->lhs->loc(), leftTruthy.error().message);
  }
  bool isTruthy = leftTruthy.isOk();
  const Type *leftType = binary->lhs->type;
  const Flow &rightStart = isAnd ? lhsTrue : lhsFalse;
  this->state.getFlowState().replaceFlow(rightStart);
  bool isPlainHint = isAnd || leftType->isError() || leftType->isUndefined();
  const Type *hint = isPlainHint ? expected : leftType->nonOptional();
  if (side == RightSide::TESTED) {
    bool isRightTruthy = this->walkTestedRight(binary, hint, rhsTrue, rhsFalse);
    isTruthy = isTruthy && isRightTruthy;
  } else {
    binary->rhs = this->exprChecker->checkExpr(binary->rhs, hint);
    rhsTrue = this->state.getFlowState().getFlow();
    rhsFalse = this->state.getFlowState().getFlow();
  }
  whenTrue = isAnd ? rhsTrue : this->flowChecker->join(lhsTrue, rhsTrue);
  whenFalse = isAnd ? this->flowChecker->join(lhsFalse, rhsFalse) : rhsFalse;
  auto joined = this->flowChecker->join(whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(joined);
  if (!isAssign) {
    this->jsonChecker->walkRedundantTests(binary);
  }

  return this->checkLogicalType(binary, leftType, isTruthy, use);
}

bool TruthChecker::walkTestedRight(BinaryExpr *binary, const Type *hint, Flow &rhsTrue,
                                   Flow &rhsFalse) {
  binary->rhs = this->checkTestedValue(binary->rhs, hint, rhsTrue, rhsFalse, TruthUse::Condition);
  auto rightTruthy = this->checkTruthy(binary->rhs, TruthUse::Condition);
  if (rightTruthy.isErr()) {
    this->state.getDiagnostics().error(binary->rhs->loc(), rightTruthy.error().message);
  }

  return rightTruthy.isOk();
}

Expr *TruthChecker::checkLogicalType(BinaryExpr *binary, const Type *leftType, bool isTruthy,
                                     TruthUse use) {
  bool isAnd = binary->op == BinaryOp::LogAnd;
  bool isAssign = use == TruthUse::OrAssign || use == TruthUse::AndAssign;
  auto op = this->logicalSpelling(binary->op, isAssign);
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  if (!isTruthy) {
    return binary;
  }
  const auto *kept = this->keptLeftType(leftType, isAnd);
  if (leftType->isError() || rightType->isError()) {
    binary->type = this->failedLogicalType(leftType, rightType, kept);
    return binary;
  }
  if (leftType->isBool() && rightType->isBool()) {
    binary->type = leftType;
    return binary;
  }
  auto joined = this->checkLogicalValue(binary, op, isAnd, kept);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(binary->loc(), joined.error().message);
    return binary;
  }
  const Type *result = joined.value();
  binary->rhs = this->typeRelations->convert(binary->rhs, result);
  binary->type = result;

  return binary;
}

std::string TruthChecker::logicalSpelling(BinaryOp op, bool isAssign) const {
  auto spelling = std::string(Operators::opSpelling(op));
  if (!isAssign) {
    return spelling;
  }

  return spelling + kAssignSuffix;
}

const Type *TruthChecker::keptLeftType(const Type *leftType, bool isAnd) const {
  if (leftType->isError()) {
    return leftType;
  }
  if (isAnd) {
    return this->falsyType(leftType);
  }

  return leftType->nonOptional();
}

const Type *TruthChecker::failedLogicalType(const Type *leftType, const Type *rightType,
                                            const Type *kept) const {
  bool isOneFailed = leftType->isError() != rightType->isError();
  if (!this->state.getFunctionStack().isInsideTrial() || !isOneFailed) {
    return Type::error();
  }

  return leftType->isError() ? rightType : kept;
}

llvm::StringRef TruthChecker::useOperator(TruthUse use) const {
  switch (use) {
    case TruthUse::Condition:
      return "";
    case TruthUse::Not:
      return "!";
    case TruthUse::Or:
      return "||";
    case TruthUse::And:
      return "&&";
    case TruthUse::OrAssign:
      return "||=";
    case TruthUse::AndAssign:
      return "&&=";
  }

  return "";
}

std::string TruthChecker::paramMessage(const Type *type, const Type *base, TruthUse use,
                                       llvm::StringRef valueText, llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  auto what = ErrorTextTruthy::typeValue(type->name());
  auto head = ErrorTextTruthy::cannotTest(this->useOperator(use), what) +
              ErrorTextTruthy::mayBeNum(base->name());
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orDefault(valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(valueText, otherText);
  }
  if (use == TruthUse::AndAssign) {
    return head + ErrorTextTruthy::andAssign(valueText, otherText);
  }

  return head + ErrorTextTruthy::undefTest(valueText, isNegated);
}

std::string TruthChecker::jsonMessage(TruthUse use, llvm::StringRef valueText,
                                      llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  auto head = ErrorTextTruthy::jsonHead(this->useOperator(use)) + ErrorTextTruthy::jsonMay();
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::jsonOr(valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(valueText, otherText);
  }

  return head + ErrorTextTruthy::jsonTest(valueText, isNegated);
}

std::string TruthChecker::unknownMessage(TruthUse use, llvm::StringRef valueText,
                                         llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  auto head = ErrorTextTruthy::unknownHd(this->useOperator(use)) + ErrorTextTruthy::unknownMay();
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orDefault(valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(valueText, otherText);
  }

  return head + ErrorTextTruthy::unknownTst(valueText, isNegated);
}

std::string TruthChecker::numberMessage(const Expr *expr, const Type *type, const Type *base,
                                        TruthUse use, llvm::StringRef valueText,
                                        llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  bool isLength = this->isLength(expr);
  auto what = ErrorTextTruthy::number(base->isDecimal(), type->isOptional());
  auto head = ErrorTextTruthy::cannotTest(this->useOperator(use), what) + ErrorTextTruthy::zero();
  if (type->isOptional()) {
    auto advice = this->optionalNumberAdvice(use, isLength, valueText, otherText);

    return head + ErrorTextTruthy::too() + advice;
  }
  if (base->isFloat()) {
    return head + ErrorTextTruthy::floatTest(valueText, isNegated);
  }

  auto test = ErrorTextTruthy::zeroTest(valueText, isNegated, isLength);
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orCompare(test, valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssignIf(valueText, otherText);
  }
  if (use == TruthUse::AndAssign) {
    return head + ErrorTextTruthy::andIf(test, valueText, otherText);
  }

  return head + ErrorTextTruthy::compare(test);
}

std::string TruthChecker::optionalNumberAdvice(TruthUse use, bool isLength,
                                               llvm::StringRef valueText,
                                               llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  bool isLengthUse =
      isLength && (use == TruthUse::Condition || use == TruthUse::Not || use == TruthUse::And);
  if (isLengthUse) {
    return ErrorTextTruthy::lengthTest(valueText, isNegated);
  }
  if (use == TruthUse::Or) {
    return ErrorTextTruthy::orDefault(valueText, otherText);
  }
  if (use == TruthUse::OrAssign) {
    return ErrorTextTruthy::orAssign(valueText, otherText);
  }

  return ErrorTextTruthy::undefTest(valueText, isNegated);
}

std::string TruthChecker::objectMessage(const Expr *expr, const Type *base, TruthUse use,
                                        llvm::StringRef valueText,
                                        llvm::StringRef otherText) const {
  auto truth = this->objectTruth(expr, base);
  auto message = this->objectHead(truth, use);
  auto advice = this->objectAdvice(truth, base, use, valueText, otherText);

  return message + advice;
}

TruthChecker::ObjectTruth TruthChecker::objectTruth(const Expr *expr, const Type *base) const {
  bool isClass = base->isClassRef();
  bool isPromise = base->isPromise();
  const auto *bare = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(bare);
  bool isClassName = isClass && name != nullptr && name->cls() != nullptr && name->var() == nullptr;

  return ObjectTruth{
      .narrowed = this->narrowedVariable(expr),
      .isArray = base->isArray(),
      .isPromise = isPromise,
      .isClass = isClass,
      .isObject = base->isObject() || base->isIterator() || base->isDict() || isClass || isPromise,
      .isClassName = isClassName,
  };
}

std::string TruthChecker::objectHead(const ObjectTruth &truth, TruthUse use) const {
  auto what = ErrorTextTruthy::kind(truth.isArray, truth.isPromise, truth.isClass, truth.isObject);
  auto subject = this->objectSubject(truth, what);
  auto consequence = this->useConsequence(use);

  return subject + consequence;
}

std::string TruthChecker::objectSubject(const ObjectTruth &truth, llvm::StringRef what) const {
  if (truth.narrowed != nullptr) {
    return ErrorTextTruthy::holds(truth.narrowed->name, what);
  }
  if (truth.isArray) {
    return ErrorTextTruthy::arrayTrue();
  }

  return ErrorTextTruthy::alwaysTrue(what);
}

std::string TruthChecker::useConsequence(TruthUse use) const {
  switch (use) {
    case TruthUse::Condition:
      return ErrorTextTruthy::soAlways();
    case TruthUse::Not:
      return ErrorTextTruthy::soNot();
    case TruthUse::Or:
      return ErrorTextTruthy::soOr();
    case TruthUse::And:
      return ErrorTextTruthy::soAnd();
    case TruthUse::OrAssign:
      return ErrorTextTruthy::soOrSet();
    case TruthUse::AndAssign:
      return ErrorTextTruthy::soAndSet();
  }

  return "";
}

std::string TruthChecker::objectAdvice(const ObjectTruth &truth, const Type *base, TruthUse use,
                                       llvm::StringRef valueText, llvm::StringRef otherText) const {
  bool isNegated = use == TruthUse::Not;
  auto length = ErrorTextTruthy::lengthCmp(valueText, isNegated);
  bool isNamedObject = truth.narrowed != nullptr || truth.isObject;
  bool isKnownHolder = truth.narrowed != nullptr || truth.isClassName;
  if (use == TruthUse::AndAssign) {
    return ErrorTextTruthy::useAssign();
  }
  if (use == TruthUse::Or) {
    return ErrorTextTruthy::dropOr(otherText);
  }
  if (use == TruthUse::OrAssign) {
    return truth.isArray ? ErrorTextTruthy::arrayLen(valueText, otherText)
                         : ErrorTextTruthy::dropAssign();
  }
  if (isNamedObject && use == TruthUse::And) {
    return ErrorTextTruthy::dropAnd(valueText);
  }
  if (isKnownHolder) {
    return truth.isArray ? ErrorTextTruthy::dropOrLen(length) : ErrorTextTruthy::dropCheck();
  }
  if (truth.isArray) {
    return ErrorTextTruthy::checkLen(length);
  }
  if (truth.isObject) {
    return ErrorTextTruthy::allowUndef(base->name());
  }
  bool isWithoutArgs = base->minArgs() == 0;
  auto call = ErrorTextTruthy::callOf(valueText, isNegated, isWithoutArgs);

  return ErrorTextTruthy::callIt(call);
}

TruthChecker::ResultTruthIsTested TruthChecker::checkTruthy(const Expr *expr, TruthUse use,
                                                            const Expr *other) {
  const Type *type = expr->type;
  const auto *declared = type->nonOptional();
  const auto *base = this->genericChecker->apparentType(declared);
  auto problem = this->truthProblem(type, base);
  if (!problem.has_value()) {
    return ok(expr);
  }
  auto message = this->truthMessage(*problem, expr, use, other, base);

  return err(TruthCheckerCheckTruthyError{
      .code = *problem,
      .message = message,
  });
}

std::optional<TruthCheckerCheckTruthyErrorCode> TruthChecker::truthProblem(const Type *type,
                                                                           const Type *base) const {
  if (this->isTestable(type, base)) {
    return std::nullopt;
  }
  if (base->isUnion() || base->isLiteral()) {
    return this->unionProblem(type, base);
  }
  if (type->isVoid()) {
    return TruthCheckerCheckTruthyErrorCode::VOID_VALUE;
  }
  if (type->isUndefined()) {
    return TruthCheckerCheckTruthyErrorCode::ALWAYS_UNDEFINED;
  }
  if (base->isParam()) {
    return TruthCheckerCheckTruthyErrorCode::PARAM_MAY_BE_NUMBER;
  }
  if (base->isJson()) {
    return TruthCheckerCheckTruthyErrorCode::JSON_VALUE;
  }
  if (base->isUnknown()) {
    return TruthCheckerCheckTruthyErrorCode::UNKNOWN_VALUE;
  }
  if (base->isResult()) {
    return TruthCheckerCheckTruthyErrorCode::RESULT_VALUE;
  }
  if (base->isNumeric() || base->isDecimal()) {
    return TruthCheckerCheckTruthyErrorCode::NUMBER_VALUE;
  }

  return TruthCheckerCheckTruthyErrorCode::ALWAYS_TRUTHY;
}

bool TruthChecker::isTestable(const Type *type, const Type *base) const {
  if (type->isUndefined() || base->isNever()) {
    return true;
  }
  bool isHolder = base->isArray() || base->isFunction() || base->isObject() || base->isResult() ||
                  base->isIterator() || base->isDict() || base->isPromise() || base->isClassRef();
  bool isOptionalObject = type->isOptional() && isHolder;
  if (type->isError() || base->isBool() || base->isString() || isOptionalObject) {
    return true;
  }

  return base->isParam() && !base->alternatives().empty() &&
         this->areAllBoolOrString(base->alternatives());
}

bool TruthChecker::areAllBoolOrString(llvm::ArrayRef<const Type *> types) const {
  for (const Type *type : types) {
    if (!type->isBool() && !type->isString()) {
      return false;
    }
  }

  return true;
}

std::optional<TruthCheckerCheckTruthyErrorCode> TruthChecker::unionProblem(const Type *type,
                                                                           const Type *base) const {
  const auto *number = this->unionNumber(base);
  bool isFalsy = type->isOptional() || this->hasFalsyMember(base);
  if (number == nullptr && isFalsy) {
    return std::nullopt;
  }
  if (number == nullptr) {
    return TruthCheckerCheckTruthyErrorCode::UNION_ALWAYS_TRUTHY;
  }

  return TruthCheckerCheckTruthyErrorCode::UNION_WITH_NUMBER;
}

std::vector<const Type *> TruthChecker::unionMembers(const Type *base) const {
  if (!base->isUnion()) {
    return {base};
  }
  auto members = base->members();

  return std::vector<const Type *>(members.begin(), members.end());
}

const Type *TruthChecker::unionNumber(const Type *base) const {
  const Type *number = nullptr;
  for (const Type *member : this->unionMembers(base)) {
    if (this->isNumberMember(member)) {
      number = member;
    }
  }

  return number;
}

bool TruthChecker::isNumberMember(const Type *member) const {
  const Type *plain = member->isLiteral() ? member->literalBase() : member;

  return plain->isNumeric() || plain->isDecimal() || plain->isParam() || plain->isJson();
}

bool TruthChecker::hasFalsyMember(const Type *base) const {
  for (const Type *member : this->unionMembers(base)) {
    if (this->isFalsyMember(member)) {
      return true;
    }
  }

  return false;
}

bool TruthChecker::isFalsyMember(const Type *member) const {
  if (!member->isLiteral()) {
    return member->isBool() || member->isString();
  }
  if (member->isStringLiteral()) {
    return member->literalText().empty();
  }

  return member->literalNumber() == 0;
}

std::string TruthChecker::truthMessage(TruthCheckerCheckTruthyErrorCode code, const Expr *expr,
                                       TruthUse use, const Expr *other, const Type *base) const {
  const Type *type = expr->type;
  bool isNegated = use == TruthUse::Not;
  auto valueText = this->hintText(expr);
  auto otherText = this->hintText(other, kEllipsis);
  switch (code) {
    case TruthCheckerCheckTruthyErrorCode::UNION_ALWAYS_TRUTHY:
      return ErrorTextTruthy::unionTrue(type->name());
    case TruthCheckerCheckTruthyErrorCode::UNION_WITH_NUMBER:
      return this->unionNumberMessage(type, base, valueText);
    case TruthCheckerCheckTruthyErrorCode::VOID_VALUE:
      return ErrorTextTruthy::voidValue();
    case TruthCheckerCheckTruthyErrorCode::ALWAYS_UNDEFINED:
      return ErrorTextTruthy::undefined();
    case TruthCheckerCheckTruthyErrorCode::PARAM_MAY_BE_NUMBER:
      return this->paramMessage(type, base, use, valueText, otherText);
    case TruthCheckerCheckTruthyErrorCode::JSON_VALUE:
      return this->jsonMessage(use, valueText, otherText);
    case TruthCheckerCheckTruthyErrorCode::UNKNOWN_VALUE:
      return this->unknownMessage(use, valueText, otherText);
    case TruthCheckerCheckTruthyErrorCode::RESULT_VALUE:
      return ErrorTextTruthy::result(valueText, isNegated);
    case TruthCheckerCheckTruthyErrorCode::NUMBER_VALUE:
      return this->numberMessage(expr, type, base, use, valueText, otherText);
    case TruthCheckerCheckTruthyErrorCode::ALWAYS_TRUTHY:
      return this->objectMessage(expr, base, use, valueText, otherText);
  }

  return this->objectMessage(expr, base, use, valueText, otherText);
}

std::string TruthChecker::unionNumberMessage(const Type *type, const Type *base,
                                             llvm::StringRef value) const {
  const auto *number = this->unionNumber(base);

  return ErrorTextTruthy::unionNum(type->name(), number->name(), value);
}

TruthChecker::ResultConditionForm TruthChecker::checkAssignmentCondition(const AssignExpr *assign,
                                                                         TruthUse use) const {
  bool isNegated = use == TruthUse::Not;
  auto target = this->hintText(assign->target);
  auto value = this->hintText(assign->value, kEllipsis);
  const Type *type = assign->type;
  bool isTestable =
      type->isOptional() || type->isNumeric() || type->isDecimal() || type->isString();
  bool isCompareInstead = this->isLiteral(assign->value) || type->isError() || !isTestable;
  if (isCompareInstead) {
    const char *equals = Operators::opSpelling(isNegated ? BinaryOp::Ne : BinaryOp::Eq);
    auto message = ErrorTextAssignTest::compare(equals, target, value);

    return err(TruthCheckerAssignmentConditionError{
        .code = TruthCheckerAssignmentConditionErrorCode::COMPARE_INSTEAD,
        .message = message,
    });
  }

  auto message = ErrorTextAssignTest::testValue(target, value, isNegated, type->isOptional(),
                                                type->isString());

  return err(TruthCheckerAssignmentConditionError{
      .code = TruthCheckerAssignmentConditionErrorCode::TEST_ASSIGNED,
      .message = message,
  });
}

TruthChecker::ResultLogicalJoined TruthChecker::checkLogicalValue(const BinaryExpr *binary,
                                                                  llvm::StringRef op, bool isAnd,
                                                                  const Type *kept) const {
  const Type *rightType = binary->rhs->type;
  if (rightType->isVoid()) {
    auto test = this->hintText(binary->lhs);
    auto right = this->hintText(binary->rhs, kEllipsis);
    auto message = ErrorTextLogical::rightVoid(op, isAnd, test, right);

    return err(TruthCheckerLogicalValueError{
        .code = TruthCheckerLogicalValueErrorCode::RIGHT_SIDE_VOID,
        .message = message,
    });
  }

  const auto *result = this->typeRelations->unify(kept, rightType);
  if (result == nullptr) {
    auto message = ErrorTextLogical::noCommon(op, kept->name(), isAnd, rightType->name());

    return err(TruthCheckerLogicalValueError{
        .code = TruthCheckerLogicalValueErrorCode::NO_COMMON_TYPE,
        .message = message,
    });
  }

  return ok(result);
}

bool TruthChecker::isBooleanCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }
  bool isSingleArg = !call->optional && call->args.size() == 1 && !call->hasSpread();

  return isSingleArg && this->callChecker->isGlobalName(call->callee, GlobalName::BOOLEAN);
}

Expr *TruthChecker::checkBooleanCall(CallExpr *call, Flow &whenTrue, Flow &whenFalse) {
  return this->checkBranching(call->args[0], whenTrue, whenFalse);
}

}
