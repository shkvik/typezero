#include "compiler/semantic/code/logic/logic_branches/logic_branches.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kEllipsis = "...";

}

LogicBranches::LogicBranches(SemanticState &owner, LogicRules &sharedLogicRules)
    : state(owner),
      logicRules(sharedLogicRules) {}

void LogicBranches::checkCondition(const CheckCondition &task) {
  this->state.walk.tasks.pushTask(FinishCondition{});

  return this->state.walk.tasks.pushTask(
      StartBranch{.expr = task.cond, .use = TruthUse::Condition});
}

void LogicBranches::finishCondition() {
  Expr *checked = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  flows.constant = this->instanceConstant(checked);
  this->state.walk.tasks.pushChecked(checked);

  return this->state.walk.tasks.pushConditionFlows(std::move(flows));
}

void LogicBranches::startBranch(const StartBranch &task) {
  Expr *expr = task.expr;
  auto *binary = dyn_cast<BinaryExpr>(expr);
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *assign = dyn_cast<AssignExpr>(expr);
  if (binary != nullptr && (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr)) {
    this->state.walk.tasks.pushTask(AfterBranchLeft{.binary = binary});
    return this->state.walk.tasks.pushTask(
        StartBranch{.expr = binary->lhs, .use = TruthUse::Condition});
  }
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    this->state.walk.tasks.pushTask(AfterBranchNot{.unary = unary});
    return this->state.walk.tasks.pushTask(
        StartBranch{.expr = unary->operand, .use = TruthUse::Not});
  }
  if (this->isBooleanCall(expr)) {
    auto *call = cast<CallExpr>(expr);
    return this->state.walk.tasks.pushTask(
        StartBranch{.expr = call->args[0], .use = TruthUse::Condition});
  }
  if (assign != nullptr && !assign->op.has_value()) {
    this->state.walk.tasks.pushTask(AfterAssignedLeaf{.assign = assign, .use = task.use});
    return this->state.walk.tasks.pushTask(CheckExpr{.expr = assign, .expected = nullptr});
  }
  this->state.walk.tasks.pushTask(AfterBranchLeaf{.use = task.use});

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = expr, .expected = Type::boolTy()});
}

void LogicBranches::afterBranchLeaf(const AfterBranchLeaf &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushTask(FinishBranchLeaf{.checked = checked, .use = task.use});

  return this->logicRules.pushValueFacts(checked);
}

void LogicBranches::finishBranchLeaf(const FinishBranchLeaf &task) {
  Expr *checked = task.checked;
  if (!checked->type->isBool()) {
    auto truthy = this->logicRules.checkTruthy(checked, task.use);
    if (truthy.isErr()) {
      this->state.diagnostics.error(checked->loc(), truthy.error().message);
    }
  }
  this->state.walk.tasks.pushChecked(checked);

  return this->state.walk.tasks.pushTask(Convert{.to = Type::boolTy()});
}

void LogicBranches::afterAssignedLeaf(const AfterAssignedLeaf &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushConditionFlows(ConditionFlows{
      .whenTrue = this->state.walk.flow.getFlow(),
      .whenFalse = this->state.walk.flow.getFlow(),
      .constant = std::nullopt,
  });
  auto form = this->checkAssignmentCondition(task.assign, task.use);
  if (form.isErr()) {
    this->state.diagnostics.error(task.assign->loc(), form.error().message);
  }
  this->state.walk.tasks.pushChecked(checked);

  return this->state.walk.tasks.pushTask(Convert{.to = Type::boolTy()});
}

void LogicBranches::afterBranchLeft(const AfterBranchLeft &task) {
  BinaryExpr *binary = task.binary;
  Expr *left = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  binary->lhs = left;
  bool isAnd = binary->op == BinaryOp::LogAnd;
  auto constant = this->instanceConstant(binary->lhs);
  if (constant.has_value() && *constant != isAnd) {
    auto joined = this->state.flow.joins.join(flows.whenTrue, flows.whenFalse);
    this->state.walk.flow.replaceFlow(std::move(joined));
    this->state.walk.tasks.pushChecked(left);
    return this->state.walk.tasks.pushConditionFlows(std::move(flows));
  }
  const Flow &rightStart = isAnd ? flows.whenTrue : flows.whenFalse;
  this->state.walk.flow.replaceFlow(rightStart);
  this->state.walk.tasks.pushTask(AfterBranchRight{
      .binary = binary,
      .lhsTrue = std::move(flows.whenTrue),
      .lhsFalse = std::move(flows.whenFalse),
  });

  return this->state.walk.tasks.pushTask(
      StartBranch{.expr = binary->rhs, .use = TruthUse::Condition});
}

void LogicBranches::afterBranchRight(const AfterBranchRight &task) {
  BinaryExpr *binary = task.binary;
  binary->rhs = this->state.walk.tasks.takeChecked();
  auto right = this->state.walk.tasks.takeConditionFlows();
  bool isAnd = binary->op == BinaryOp::LogAnd;
  const auto &joins = this->state.flow.joins;
  auto whenTrue = isAnd ? right.whenTrue : joins.join(task.lhsTrue, right.whenTrue);
  auto whenFalse = isAnd ? joins.join(task.lhsFalse, right.whenFalse) : right.whenFalse;
  auto joined = joins.join(whenTrue, whenFalse);
  this->state.walk.flow.replaceFlow(std::move(joined));
  binary->type = Type::boolTy();
  this->state.walk.tasks.pushChecked(binary);
  this->state.walk.tasks.pushConditionFlows(ConditionFlows{
      .whenTrue = std::move(whenTrue),
      .whenFalse = std::move(whenFalse),
      .constant = std::nullopt,
  });

  return this->state.walk.tasks.pushTask(WalkRedundantTests{.binary = binary});
}

void LogicBranches::afterBranchNot(const AfterBranchNot &task) {
  UnaryExpr *unary = task.unary;
  unary->operand = this->state.walk.tasks.takeChecked();
  auto operand = this->state.walk.tasks.takeConditionFlows();
  unary->type = Type::boolTy();
  this->state.walk.tasks.pushChecked(unary);

  return this->state.walk.tasks.pushConditionFlows(ConditionFlows{
      .whenTrue = std::move(operand.whenFalse),
      .whenFalse = std::move(operand.whenTrue),
      .constant = std::nullopt,
  });
}

void LogicBranches::checkNot(const CheckNot &task) {
  this->state.walk.tasks.pushTask(FinishBranchJoin{});

  return this->state.walk.tasks.pushTask(
      StartBranch{.expr = task.unary, .use = TruthUse::Condition});
}

void LogicBranches::finishBranchJoin() {
  auto flows = this->state.walk.tasks.takeConditionFlows();
  auto joined = this->state.flow.joins.join(flows.whenTrue, flows.whenFalse);

  return this->state.walk.flow.replaceFlow(std::move(joined));
}

void LogicBranches::startTestedValue(const StartTestedValue &task) {
  Expr *expr = task.expr;
  auto *binary = dyn_cast<BinaryExpr>(expr);
  auto *unary = dyn_cast<UnaryExpr>(expr);
  if (binary != nullptr && (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr)) {
    auto logicalUse = binary->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or;
    auto logical = StartLogical{
        .binary = binary,
        .expected = task.expected,
        .side = RightSide::TESTED,
        .use = logicalUse,
    };
    return this->startLogical(logical);
  }
  bool isNot = unary != nullptr && unary->op == UnaryOp::Not;
  if (isNot || this->isBooleanCall(expr)) {
    auto branch = StartBranch{.expr = expr, .use = task.use};
    return this->startBranch(branch);
  }
  this->state.walk.tasks.pushTask(AfterTestedLeaf{});

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = expr, .expected = task.expected});
}

void LogicBranches::startLogical(const StartLogical &task) {
  auto draft = LogicalDraft{
      .binary = task.binary,
      .expected = task.expected,
      .side = task.side,
      .use = task.use,
      .lhsTrue = Flow(),
      .lhsFalse = Flow(),
      .leftType = nullptr,
      .isTruthy = false,
  };
  this->state.walk.tasks.pushTask(AfterLogicalLeft{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(StartTestedValue{
      .expr = task.binary->lhs,
      .expected = task.expected,
      .use = task.use,
  });
}

std::optional<bool> LogicBranches::instanceConstant(const Expr *cond) const {
  if (!this->state.generics.copies.isCheckingInstance()) {
    return std::nullopt;
  }
  auto answers = std::vector<std::optional<bool>>();
  for (const Expr *node : ChildrenFirst::order(cond, this, &LogicBranches::conditionParts)) {
    auto value = this->nodeCondition(node, answers);
    answers.push_back(value);
  }

  return answers.back();
}

NodeChildren<const Expr *> LogicBranches::conditionParts(const Expr *cond) const {
  const auto *conversion = dyn_cast<CastExpr>(cond);
  if (conversion != nullptr && this->isBoolPass(conversion)) {
    return {conversion->operand};
  }
  const auto *unary = dyn_cast<UnaryExpr>(cond);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    return {unary->operand};
  }
  const auto *binary = dyn_cast<BinaryExpr>(cond);
  bool isLogic =
      binary != nullptr && (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr);
  if (isLogic) {
    return {binary->lhs, binary->rhs};
  }

  return {};
}

bool LogicBranches::isBoolPass(const CastExpr *conversion) const {
  return conversion->isImplicit() && conversion->type->isBool() &&
         conversion->operand->type->isBool();
}

std::optional<bool> LogicBranches::nodeCondition(const Expr *cond,
                                                 std::vector<std::optional<bool>> &answers) const {
  const auto *conversion = dyn_cast<CastExpr>(cond);
  if (conversion != nullptr && this->isBoolPass(conversion)) {
    return ChildrenFirst::takeAnswer(answers);
  }
  const auto *literal = dyn_cast<BoolLitExpr>(cond);
  if (literal != nullptr) {
    return literal->value;
  }
  const auto *unary = dyn_cast<UnaryExpr>(cond);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    std::optional<bool> value = ChildrenFirst::takeAnswer(answers);
    return value.has_value() ? std::optional<bool>(!*value) : std::nullopt;
  }
  const auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::JsonIsJson) {
    return this->isJsonCondition(call);
  }
  const auto *binary = dyn_cast<BinaryExpr>(cond);
  if (binary == nullptr) {
    return std::nullopt;
  }
  if (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr) {
    return this->logicalCondition(binary, answers);
  }
  if (binary->op != BinaryOp::Eq && binary->op != BinaryOp::Ne) {
    return std::nullopt;
  }
  auto undefinedCheck = this->undefinedCondition(binary);
  if (undefinedCheck.has_value()) {
    return undefinedCheck;
  }

  return this->typeOfCondition(binary);
}

std::optional<bool> LogicBranches::isJsonCondition(const CallExpr *call) const {
  const Type *type = call->args[0]->type;
  if (type->isUnknown() || type->nonOptional()->isParam()) {
    return std::nullopt;
  }

  return type->nonOptional()->isJson();
}

std::optional<bool>
LogicBranches::logicalCondition(const BinaryExpr *binary,
                                std::vector<std::optional<bool>> &answers) const {
  std::optional<bool> right = ChildrenFirst::takeAnswer(answers);
  std::optional<bool> left = ChildrenFirst::takeAnswer(answers);
  bool isAnd = binary->op == BinaryOp::LogAnd;
  if (left.has_value() && *left != isAnd) {
    return left;
  }
  if (left.has_value()) {
    return right;
  }

  return std::nullopt;
}

std::optional<bool> LogicBranches::undefinedCondition(const BinaryExpr *binary) const {
  auto leftSide = this->undefinedSide(binary, binary->lhs, binary->rhs);
  if (leftSide.has_value()) {
    return leftSide;
  }

  return this->undefinedSide(binary, binary->rhs, binary->lhs);
}

std::optional<bool> LogicBranches::undefinedSide(const BinaryExpr *binary, const Expr *side,
                                                 const Expr *other) const {
  const Expr *operand = ExprParts::withoutImplicitCasts(side);
  const Type *type = operand->type;
  bool isAlwaysUndefined = type->isUndefined() || (type->isOptional() && type->inner()->isNever());
  if (isa<UndefinedExpr>(other) && isa<NameExpr>(operand) && isAlwaysUndefined) {
    return binary->op == BinaryOp::Eq;
  }

  return std::nullopt;
}

std::optional<bool> LogicBranches::typeOfCondition(const BinaryExpr *binary) const {
  if (this->isTypeOfTest(binary->lhs, binary->rhs)) {
    return this->typeOfSide(binary, binary->lhs, binary->rhs);
  }
  if (this->isTypeOfTest(binary->rhs, binary->lhs)) {
    return this->typeOfSide(binary, binary->rhs, binary->lhs);
  }

  return std::nullopt;
}

bool LogicBranches::isTypeOfTest(const Expr *side, const Expr *other) const {
  const auto *typeOf = dyn_cast<UnaryExpr>(side);

  return typeOf != nullptr && typeOf->op == UnaryOp::TypeOf && isa<StringLitExpr>(other);
}

std::optional<bool> LogicBranches::typeOfSide(const BinaryExpr *binary, const Expr *side,
                                              const Expr *other) const {
  const auto *typeOf = cast<UnaryExpr>(side);
  const auto *text = cast<StringLitExpr>(other);
  const Expr *operand = ExprParts::withoutImplicitCasts(typeOf->operand);
  if (!isa<NameExpr>(operand)) {
    return std::nullopt;
  }

  return this->typeOfAnswer(binary, typeOf->operand->type, text);
}

std::optional<bool> LogicBranches::typeOfAnswer(const BinaryExpr *binary, const Type *type,
                                                const StringLitExpr *text) const {
  llvm::StringRef name = TypeSpelling::typeOfName(type->nonOptional());
  if (name.empty()) {
    return std::nullopt;
  }
  std::u16string wanted(name.begin(), name.end());
  bool isEqual = text->value == wanted;
  auto typeOf = TypeOfNameTable::find(ToUtf8::toUtf8(text->value));
  bool isUndefinedText = typeOf == TypeOfName::UNDEFINED_TYPE;
  if (type->isOptional() && (isUndefinedText || isEqual)) {
    return std::nullopt;
  }

  return binary->op == BinaryOp::Eq ? isEqual : !isEqual;
}

bool LogicBranches::isLiteral(const Expr *expr) const {
  const auto *bare = ExprParts::withoutCasts(expr);
  auto *unary = dyn_cast<UnaryExpr>(bare);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *value = isNegated ? unary->operand : bare;

  return isa<IntLitExpr>(value) || isa<FloatLitExpr>(value) || isa<StringLitExpr>(value) ||
         isa<BoolLitExpr>(value) || isa<UndefinedExpr>(value);
}

LogicBranches::ResultConditionForm LogicBranches::checkAssignmentCondition(const AssignExpr *assign,
                                                                           TruthUse use) const {
  bool isNegated = use == TruthUse::Not;
  auto target = this->logicRules.hintText(assign->target);
  auto value = this->logicRules.hintText(assign->value, kEllipsis);
  const Type *type = assign->type;
  bool isTestable =
      type->isOptional() || type->isNumeric() || type->isDecimal() || type->isString();
  bool isCompareInstead = this->isLiteral(assign->value) || type->isError() || !isTestable;
  if (isCompareInstead) {
    const char *equals = Operators::opSpelling(isNegated ? BinaryOp::Ne : BinaryOp::Eq);
    auto message = ErrorTextAssignTest::compare(equals, target, value);

    return err(CodeLogicRegistryAssignmentConditionError{
        .code = CodeLogicRegistryAssignmentConditionErrorCode::COMPARE_INSTEAD,
        .message = message,
    });
  }

  auto message = ErrorTextAssignTest::testValue(target, value, isNegated, type->isOptional(),
                                                type->isString());

  return err(CodeLogicRegistryAssignmentConditionError{
      .code = CodeLogicRegistryAssignmentConditionErrorCode::TEST_ASSIGNED,
      .message = message,
  });
}

bool LogicBranches::isBooleanCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }
  bool isSingleArg = !call->optional && call->args.size() == 1 && !call->hasSpread();

  return isSingleArg && this->state.isGlobalName(call->callee, GlobalName::BOOLEAN);
}

}
