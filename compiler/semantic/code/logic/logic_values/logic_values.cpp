#include "compiler/semantic/code/logic/logic_values/logic_values.h"

using llvm::cast;

namespace tz {

namespace {

constexpr const char *kEllipsis = "...";
constexpr const char *kAssignSuffix = "=";

}

LogicValues::LogicValues(SemanticState &owner, LogicRules &sharedLogicRules)
    : state(owner),
      logicRules(sharedLogicRules) {}

void LogicValues::checkLogicalOperator(const CheckLogicalOperator &task) {
  TruthUse use = task.binary->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or;
  this->state.walk.tasks.pushTask(FinishBranchJoin{});

  return this->state.walk.tasks.pushTask(StartLogical{
      .binary = task.binary,
      .expected = task.expected,
      .side = RightSide::VALUE,
      .use = use,
  });
}

void LogicValues::afterTestedLeaf() {
  Expr *checked = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushChecked(checked);

  return this->logicRules.pushValueFacts(checked);
}

void LogicValues::afterLogicalLeft(const AfterLogicalLeft &task) {
  auto draft = task.draft;
  Expr *left = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  draft.binary->lhs = left;
  draft.lhsTrue = std::move(flows.whenTrue);
  draft.lhsFalse = std::move(flows.whenFalse);
  const Type *apparent = this->state.generics.apparent.apparentType(left->type);
  Expr **slot = &draft.binary->lhs;
  this->state.walk.tasks.pushTask(StartLogicalRight{.draft = std::move(draft)});
  if (apparent != left->type) {
    this->state.walk.tasks.pushTask(ConvertSlot{.slot = slot, .to = apparent});
  }
}

void LogicValues::startLogicalRight(const StartLogicalRight &task) {
  auto draft = task.draft;
  BinaryExpr *binary = draft.binary;
  bool isAnd = binary->op == BinaryOp::LogAnd;
  bool isAssign = draft.use == TruthUse::OrAssign || draft.use == TruthUse::AndAssign;
  const Expr *other = isAssign ? cast<AssignExpr>(binary->rhs)->value : binary->rhs;
  auto leftTruthy = this->logicRules.checkTruthy(binary->lhs, draft.use, other);
  if (leftTruthy.isErr()) {
    this->state.diagnostics.error(binary->lhs->loc(), leftTruthy.error().message);
  }
  draft.isTruthy = leftTruthy.isOk();
  draft.leftType = binary->lhs->type;
  const Flow &rightStart = isAnd ? draft.lhsTrue : draft.lhsFalse;
  this->state.walk.flow.replaceFlow(rightStart);
  const Type *leftType = draft.leftType;
  bool isPlainHint = isAnd || leftType->isError() || leftType->isUndefined();
  const Type *hint = isPlainHint ? draft.expected : leftType->nonOptional();
  if (draft.side == RightSide::TESTED) {
    this->state.walk.tasks.pushTask(FinishTestedLogical{.draft = std::move(draft)});
    return this->state.walk.tasks.pushTask(StartTestedValue{
        .expr = binary->rhs,
        .expected = hint,
        .use = TruthUse::Condition,
    });
  }
  this->state.walk.tasks.pushTask(FinishValueLogical{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = binary->rhs, .expected = hint});
}

void LogicValues::finishTestedLogical(const FinishTestedLogical &task) {
  auto draft = task.draft;
  draft.binary->rhs = this->state.walk.tasks.takeChecked();
  auto right = this->state.walk.tasks.takeConditionFlows();
  auto rightTruthy = this->logicRules.checkTruthy(draft.binary->rhs, TruthUse::Condition);
  if (rightTruthy.isErr()) {
    this->state.diagnostics.error(draft.binary->rhs->loc(), rightTruthy.error().message);
  }
  draft.isTruthy = draft.isTruthy && rightTruthy.isOk();

  return this->joinLogicalFlows(std::move(draft), std::move(right));
}

void LogicValues::finishValueLogical(const FinishValueLogical &task) {
  auto draft = task.draft;
  draft.binary->rhs = this->state.walk.tasks.takeChecked();
  auto right = ConditionFlows{
      .whenTrue = this->state.walk.flow.getFlow(),
      .whenFalse = this->state.walk.flow.getFlow(),
      .constant = std::nullopt,
  };

  return this->joinLogicalFlows(std::move(draft), std::move(right));
}

void LogicValues::joinLogicalFlows(LogicalDraft draft, ConditionFlows right) {
  BinaryExpr *binary = draft.binary;
  bool isAnd = binary->op == BinaryOp::LogAnd;
  bool isAssign = draft.use == TruthUse::OrAssign || draft.use == TruthUse::AndAssign;
  const auto &joins = this->state.flow.joins;
  auto whenTrue = isAnd ? right.whenTrue : joins.join(draft.lhsTrue, right.whenTrue);
  auto whenFalse = isAnd ? joins.join(draft.lhsFalse, right.whenFalse) : right.whenFalse;
  auto joined = joins.join(whenTrue, whenFalse);
  this->state.walk.flow.replaceFlow(std::move(joined));
  this->state.walk.tasks.pushConditionFlows(ConditionFlows{
      .whenTrue = std::move(whenTrue),
      .whenFalse = std::move(whenFalse),
      .constant = std::nullopt,
  });
  this->state.walk.tasks.pushTask(FinishLogicalType{.draft = std::move(draft)});
  if (!isAssign) {
    this->state.walk.tasks.pushTask(WalkRedundantTests{.binary = binary});
  }
}

void LogicValues::finishLogicalType(const FinishLogicalType &task) {
  const LogicalDraft &draft = task.draft;
  BinaryExpr *binary = draft.binary;
  bool isAnd = binary->op == BinaryOp::LogAnd;
  bool isAssign = draft.use == TruthUse::OrAssign || draft.use == TruthUse::AndAssign;
  auto op = this->logicalSpelling(binary->op, isAssign);
  const Type *leftType = draft.leftType;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  if (!draft.isTruthy) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  const auto *kept = this->keptLeftType(leftType, isAnd);
  if (leftType->isError() || rightType->isError()) {
    binary->type = this->failedLogicalType(leftType, rightType, kept);
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (leftType->isBool() && rightType->isBool()) {
    binary->type = leftType;
    return this->state.walk.tasks.pushChecked(binary);
  }
  auto joined = this->checkLogicalValue(binary, op, isAnd, kept);
  if (joined.isErr()) {
    this->state.diagnostics.error(binary->loc(), joined.error().message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  const Type *result = joined.value();
  binary->type = result;
  this->state.walk.tasks.pushChecked(binary);

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = result});
}

const Type *LogicValues::falsyType(const Type *type) const {
  const Type *base = type->nonOptional();
  bool isAlwaysTruthy = base->isArray() || base->isFunction() || base->isObject() ||
                        base->isIterator() || base->isDict() || base->isClassRef();

  return isAlwaysTruthy ? Type::undefinedTy() : type;
}

std::string LogicValues::logicalSpelling(BinaryOp op, bool isAssign) const {
  auto spelling = std::string(Operators::opSpelling(op));
  if (!isAssign) {
    return spelling;
  }

  return spelling + kAssignSuffix;
}

const Type *LogicValues::keptLeftType(const Type *leftType, bool isAnd) const {
  if (leftType->isError()) {
    return leftType;
  }
  if (isAnd) {
    return this->falsyType(leftType);
  }

  return leftType->nonOptional();
}

const Type *LogicValues::failedLogicalType(const Type *leftType, const Type *rightType,
                                           const Type *kept) const {
  bool isOneFailed = leftType->isError() != rightType->isError();
  if (!this->state.walk.functions.isInsideTrial() || !isOneFailed) {
    return Type::error();
  }

  return leftType->isError() ? rightType : kept;
}

LogicValues::ResultLogicalJoined LogicValues::checkLogicalValue(const BinaryExpr *binary,
                                                                llvm::StringRef op, bool isAnd,
                                                                const Type *kept) const {
  const Type *rightType = binary->rhs->type;
  if (rightType->isVoid()) {
    auto test = this->logicRules.hintText(binary->lhs);
    auto right = this->logicRules.hintText(binary->rhs, kEllipsis);
    auto message = ErrorTextLogical::rightVoid(op, isAnd, test, right);

    return err(CodeLogicRegistryLogicalValueError{
        .code = CodeLogicRegistryLogicalValueErrorCode::RIGHT_SIDE_VOID,
        .message = message,
    });
  }

  const auto *result = this->state.types.relations.unify(kept, rightType);
  if (result == nullptr) {
    auto message = ErrorTextLogical::noCommon(op, kept->name(), isAnd, rightType->name());

    return err(CodeLogicRegistryLogicalValueError{
        .code = CodeLogicRegistryLogicalValueErrorCode::NO_COMMON_TYPE,
        .message = message,
    });
  }

  return ok(result);
}

}
