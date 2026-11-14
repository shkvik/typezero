#include "compiler/semantic/code/expressions/conditional_expressions/conditional_expressions.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ConditionalExpressions::ConditionalExpressions(SemanticState &owner)
    : state(owner) {}

void ConditionalExpressions::afterConditionTest(const AfterConditionTest &task) {
  ConditionalExpr *conditional = task.conditional;
  conditional->cond = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  if (flows.constant.has_value()) {
    const Flow &taken = *flows.constant ? flows.whenTrue : flows.whenFalse;
    Expr *branch = *flows.constant ? conditional->thenExpr : conditional->elseExpr;
    this->state.walk.flow.replaceFlow(taken);

    return this->state.walk.tasks.pushTask(CheckExpr{.expr = branch, .expected = task.expected});
  }
  bool isThenWaiting = this->isWaitingForOther(conditional->thenExpr);
  bool isElseWaiting = this->isWaitingForOther(conditional->elseExpr);
  this->state.walk.tasks.pushTask(FinishConditional{.conditional = conditional});
  if (isThenWaiting && !isElseWaiting) {
    this->state.walk.flow.replaceFlow(flows.whenFalse);
    this->state.walk.tasks.pushTask(AfterElseFirst{
        .conditional = conditional,
        .expected = task.expected,
        .whenTrue = std::move(flows.whenTrue),
    });

    return this->state.walk.tasks.pushTask(
        CheckExpr{.expr = conditional->elseExpr, .expected = task.expected});
  }
  this->state.walk.flow.replaceFlow(flows.whenTrue);
  this->state.walk.tasks.pushTask(AfterThenFirst{
      .conditional = conditional,
      .expected = task.expected,
      .whenFalse = std::move(flows.whenFalse),
      .isElseWaitingOnly = isElseWaiting && !isThenWaiting,
  });

  return this->state.walk.tasks.pushTask(
      CheckExpr{.expr = conditional->thenExpr, .expected = task.expected});
}

bool ConditionalExpressions::isWaitingForOther(const Expr *branch) const {
  return this->isLiteralShaped(branch) || this->state.code.conversions.isNumberLiteralTree(branch);
}

bool ConditionalExpressions::isLiteralShaped(const Expr *expr) const {
  auto stack = std::vector<const Expr *>{expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    if (this->isLiteralShape(next)) {
      return true;
    }
    auto choices = this->shapeChoices(next);
    stack.insert(stack.end(), choices.rbegin(), choices.rend());
  }

  return false;
}

bool ConditionalExpressions::isLiteralShape(const Expr *expr) const {
  if (isa<ObjectLitExpr>(expr) || isa<ArrayLitExpr>(expr)) {
    return true;
  }
  const auto *call = dyn_cast<CallExpr>(expr);

  return call != nullptr && this->isResultLiteral(call);
}

bool ConditionalExpressions::isResultLiteral(const CallExpr *call) const {
  const auto *name = dyn_cast<NameExpr>(call->callee);
  if (name == nullptr) {
    return false;
  }
  auto global = GlobalNameTable::find(name->name);
  bool isOkOrErr = global == GlobalName::OK || global == GlobalName::ERR;

  return isOkOrErr && call->args.size() <= 1 && !call->hasSpread() && !call->optional;
}

std::vector<const Expr *> ConditionalExpressions::shapeChoices(const Expr *expr) const {
  const auto *conditional = dyn_cast<ConditionalExpr>(expr);
  const auto *call = dyn_cast<CallExpr>(expr);

  if (conditional != nullptr) {
    return {conditional->thenExpr, conditional->elseExpr};
  }
  if (call != nullptr) {
    return this->mappedReturns(call);
  }

  return {};
}

std::vector<const Expr *> ConditionalExpressions::mappedReturns(const CallExpr *call) const {
  const auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr || call->args.size() != 1) {
    return {};
  }
  const auto *method = ArrayMethodTable::find(member->name);
  bool isMapping = method != nullptr &&
                   (method->method == ArrayMethod::MAP || method->method == ArrayMethod::FLAT_MAP);
  if (!isMapping) {
    return {};
  }
  const auto *functionExpr = dyn_cast<FunctionExpr>(call->args[0]);
  if (functionExpr == nullptr) {
    return {};
  }

  return this->findReturnValues(functionExpr->fn->body);
}

std::vector<const Expr *> ConditionalExpressions::findReturnValues(const BlockStmt *body) const {
  auto values = std::vector<const Expr *>();
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    const auto *returnStmt = dyn_cast<ReturnStmt>(placed.stmt);
    bool isOwnReturn = returnStmt != nullptr && !placed.place.isInsideFunction();
    if (isOwnReturn && returnStmt->value != nullptr) {
      values.push_back(returnStmt->value);
    }
  }

  return values;
}

const Type *ConditionalExpressions::branchHint(const Expr *literal, const Expr *other,
                                               const Type *expected) const {
  if (this->state.code.conversions.isNumberLiteralTree(literal)) {
    return this->state.code.conversions.literalHint(other, expected);
  }
  if (expected != nullptr) {
    return expected;
  }

  return other->type->isError() ? nullptr : other->type;
}

void ConditionalExpressions::afterElseFirst(const AfterElseFirst &task) {
  ConditionalExpr *conditional = task.conditional;
  conditional->elseExpr = this->state.walk.tasks.takeChecked();
  auto afterElse = this->state.walk.flow.getFlow();
  this->state.walk.flow.replaceFlow(task.whenTrue);
  const auto *hint = this->branchHint(conditional->thenExpr, conditional->elseExpr, task.expected);
  this->state.walk.tasks.pushTask(AfterThenSecond{
      .conditional = conditional,
      .afterElse = std::move(afterElse),
  });

  return this->state.walk.tasks.pushTask(
      CheckExpr{.expr = conditional->thenExpr, .expected = hint});
}

void ConditionalExpressions::afterThenSecond(const AfterThenSecond &task) {
  task.conditional->thenExpr = this->state.walk.tasks.takeChecked();
  const auto &afterThen = this->state.walk.flow.getFlow();
  auto joined = this->state.flow.joins.join(afterThen, task.afterElse);

  return this->state.walk.flow.replaceFlow(std::move(joined));
}

void ConditionalExpressions::afterThenFirst(const AfterThenFirst &task) {
  ConditionalExpr *conditional = task.conditional;
  conditional->thenExpr = this->state.walk.tasks.takeChecked();
  auto afterThen = this->state.walk.flow.getFlow();
  this->state.walk.flow.replaceFlow(task.whenFalse);
  const auto *hint = this->elseHint(conditional, task.expected, task.isElseWaitingOnly);
  this->state.walk.tasks.pushTask(AfterElseSecond{
      .conditional = conditional,
      .afterThen = std::move(afterThen),
  });

  return this->state.walk.tasks.pushTask(
      CheckExpr{.expr = conditional->elseExpr, .expected = hint});
}

void ConditionalExpressions::afterElseSecond(const AfterElseSecond &task) {
  task.conditional->elseExpr = this->state.walk.tasks.takeChecked();
  const auto &afterElse = this->state.walk.flow.getFlow();
  auto joined = this->state.flow.joins.join(task.afterThen, afterElse);

  return this->state.walk.flow.replaceFlow(std::move(joined));
}

const Type *ConditionalExpressions::elseHint(const ConditionalExpr *conditional,
                                             const Type *expected, bool isElseWaitingOnly) const {
  if (isElseWaitingOnly) {
    return this->branchHint(conditional->elseExpr, conditional->thenExpr, expected);
  }
  if (this->state.code.conversions.isNumberLiteralTree(conditional->elseExpr)) {
    return this->state.code.conversions.literalHint(conditional->thenExpr, expected);
  }

  return expected;
}

void ConditionalExpressions::finishConditional(const FinishConditional &task) {
  ConditionalExpr *conditional = task.conditional;
  const Type *thenType = conditional->thenExpr->type;
  const Type *elseType = conditional->elseExpr->type;
  conditional->type = Type::error();
  if (thenType->isError() || elseType->isError()) {
    conditional->type = this->failedBranchesType(thenType, elseType);
    return this->state.walk.tasks.pushChecked(conditional);
  }
  if (thenType->isVoid() && elseType->isVoid()) {
    conditional->type = thenType;
    return this->state.walk.tasks.pushChecked(conditional);
  }
  const auto &conversions = this->state.code.conversions;
  bool isDifferent = thenType != elseType;
  bool isThenAdapting =
      isDifferent && conversions.isLiteralAdapting(conditional->thenExpr, elseType);
  bool isElseAdapting = isDifferent && !isThenAdapting &&
                        conversions.isLiteralAdapting(conditional->elseExpr, thenType);
  this->state.walk.tasks.pushTask(JoinConditionalBranches{
      .conditional = conditional,
      .left = isThenAdapting ? elseType : thenType,
      .right = isElseAdapting ? thenType : elseType,
  });
  if (isElseAdapting) {
    this->state.walk.tasks.pushTask(
        AdaptLiteralSlot{.slot = &conditional->elseExpr, .to = thenType});
  }
  if (isThenAdapting) {
    this->state.walk.tasks.pushTask(
        AdaptLiteralSlot{.slot = &conditional->thenExpr, .to = elseType});
  }
}

void ConditionalExpressions::joinConditionalBranches(const JoinConditionalBranches &task) {
  ConditionalExpr *conditional = task.conditional;
  auto common = this->checkBranchesJoin(task.left, task.right);
  if (common.isErr()) {
    this->state.diagnostics.error(conditional->loc(), common.error().message);
    return this->state.walk.tasks.pushChecked(conditional);
  }
  conditional->type = common.value();
  this->state.walk.tasks.pushChecked(conditional);
  this->state.walk.tasks.pushTask(
      ConvertSlot{.slot = &conditional->elseExpr, .to = common.value()});

  return this->state.walk.tasks.pushTask(
      ConvertSlot{.slot = &conditional->thenExpr, .to = common.value()});
}

ConditionalExpressions::ResultBranchesJoined
ConditionalExpressions::checkBranchesJoin(const Type *left, const Type *right) const {
  const auto *common = this->state.types.literals.unionUnify(left, right);
  if (common == nullptr) {
    auto message = ErrorTextConditional::mismatch(left->name(), right->name());

    return err(CodeExpressionsRegistryCheckConditionalError{
        .code = CodeExpressionsRegistryCheckConditionalErrorCode::INCOMPATIBLE_BRANCHES,
        .message = message,
    });
  }

  return ok(common);
}

const Type *ConditionalExpressions::failedBranchesType(const Type *thenType,
                                                       const Type *elseType) const {
  bool isOneFailed = thenType->isError() != elseType->isError();
  if (!this->state.walk.functions.isInsideTrial() || !isOneFailed) {
    return Type::error();
  }

  return thenType->isError() ? elseType : thenType;
}

}
