#include "compiler/semantic/code/assignments/nullish_assigns/nullish_assigns.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

NullishAssigns::NullishAssigns(SemanticState &owner, AssignmentRules &sharedAssignmentRules)
    : state(owner),
      assignmentRules(sharedAssignmentRules) {}

void NullishAssigns::afterLogicalAssign(const AfterLogicalAssign &task) {
  this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  auto joined = this->state.flow.joins.join(flows.whenTrue, flows.whenFalse);
  this->state.walk.flow.replaceFlow(std::move(joined));

  return this->state.walk.tasks.pushChecked(task.binary);
}

void NullishAssigns::afterNullishHolder(const AfterNullishHolder &task) {
  AssignExpr *assign = task.assign;
  auto *member = cast<MemberExpr>(assign->target);
  if (!this->isRecordHolder(member->object)) {
    return this->assignmentRules.startNullishByTarget(assign);
  }
  auto *key = this->assignmentRules.keyLiteral(member->loc(), member->name);
  auto *index = this->state.ast.make<IndexExpr>(member->loc(), member->object, key);
  assign->target = index;
  this->state.walk.tasks.pushTask(AfterRecordFieldNullish{.assign = assign});

  return this->state.walk.tasks.pushTask(
      CheckRecordNullishAssign{.assign = assign, .index = index});
}

bool NullishAssigns::isRecordHolder(const Expr *object) const {
  auto *name = dyn_cast<NameExpr>(object);
  const VarSymbol *variable =
      name != nullptr ? this->state.walk.scopes.findName(name->name).var() : nullptr;
  bool isRecordVariable =
      variable != nullptr && variable->type != nullptr && variable->type->isRecord();

  return isRecordVariable ||
         this->state.library.process.isProcessMember(object, ProcessMember::ENV);
}

void NullishAssigns::afterRecordFieldNullish(const AfterRecordFieldNullish &task) {
  Expr *set = this->state.walk.tasks.takeChecked();
  if (set != nullptr) {
    return this->state.walk.tasks.pushChecked(set);
  }

  return this->assignmentRules.startNullishByTarget(task.assign);
}

void NullishAssigns::afterIndexNullish(const AfterIndexNullish &task) {
  Expr *set = this->state.walk.tasks.takeChecked();
  if (set != nullptr) {
    return this->state.walk.tasks.pushChecked(set);
  }

  return this->assignmentRules.startVariableNullish(task.assign);
}

void NullishAssigns::afterFieldNullish(const AfterFieldNullish &task) {
  Expr *result = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushChecked(result);
  auto *member = cast<MemberExpr>(task.set->target);
  auto absent = this->checkFieldOptional(member, member->type);
  if (absent.isErr()) {
    return this->state.diagnostics.error(task.assign->loc(), absent.error().message);
  }
  const auto *valueType = this->assignedValueType(task.set->value);
  if (!valueType->isOptional() && !valueType->isUndefined()) {
    return this->defineFieldOf(member);
  }
}

void NullishAssigns::afterNullishVariable(const AfterNullishVariable &task) {
  AssignExpr *assign = task.assign;
  auto *name = dyn_cast<NameExpr>(assign->target);
  const VarSymbol *variable = name != nullptr ? name->var() : nullptr;
  auto target = this->checkVariableTarget(assign->target);
  if (target.isErr()) {
    this->state.diagnostics.error(assign->target->loc(), target.error().message);
  }
  const Type *declared = variable != nullptr ? variable->type : Type::error();
  assign->type = declared;
  auto optional = this->checkNullishOptional(name, declared);
  if (optional.isErr()) {
    this->state.diagnostics.error(assign->loc(), optional.error().message);
  }
  const Type *type = optional.isOk() ? declared : Type::error();
  const Type *hint = type->isError() ? nullptr : type->inner();
  auto before = this->state.walk.flow.getFlow();
  this->state.walk.tasks.pushTask(
      FinishNullishVariable{.assign = assign, .type = type, .before = std::move(before)});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &assign->value, .expected = hint});
}

void NullishAssigns::finishNullishVariable(const FinishNullishVariable &task) {
  AssignExpr *assign = task.assign;
  const Type *type = task.type;
  const Flow &after = this->state.walk.flow.getFlow();
  auto joined = this->state.flow.joins.join(task.before, after);
  this->state.walk.flow.replaceFlow(std::move(joined));
  this->state.walk.tasks.pushChecked(assign);
  if (type->isError()) {
    return;
  }
  const Type *valueType = assign->value->type;
  bool isDefinite = !valueType->isOptional() && !valueType->isUndefined();
  const Type *stored = isDefinite ? type->inner() : type;
  auto *name = dyn_cast<NameExpr>(assign->target);
  VarSymbol *variable = name != nullptr ? name->var() : nullptr;
  if (variable != nullptr && isDefinite) {
    this->state.walk.flow.markDefined(variable);
  }
  if (variable != nullptr) {
    assign->type = stored;
  }

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = &assign->value, .to = stored, .what = ""});
}

NullishAssigns::ResultTargetIsVariable
NullishAssigns::checkVariableTarget(const Expr *target) const {
  if (!isa<NameExpr>(target)) {
    auto message = ErrorTextAssign::nullishVar();

    return err(CodeAssignmentsRegistryVariableTargetError{
        .code = CodeAssignmentsRegistryVariableTargetErrorCode::NOT_A_VARIABLE,
        .message = message,
    });
  }

  return ok(target);
}

NullishAssigns::ResultVariableOptional
NullishAssigns::checkNullishOptional(const NameExpr *name, const Type *type) const {
  if (!type->isOptional() && !type->isError()) {
    auto message = ErrorTextAssign::notOptional(name->name, type->name());

    return err(CodeAssignmentsRegistryNullishOptionalError{
        .code = CodeAssignmentsRegistryNullishOptionalErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(type);
}

NullishAssigns::ResultFieldMayBeAbsent NullishAssigns::checkFieldOptional(const MemberExpr *member,
                                                                          const Type *type) const {
  if (type != nullptr && !type->isOptional() && !type->isError()) {
    auto message = ErrorTextLogicalAssign::fieldOpt(member->name, type->name());

    return err(CodeAssignmentsRegistryFieldOptionalError{
        .code = CodeAssignmentsRegistryFieldOptionalErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(type);
}

const Type *NullishAssigns::assignedValueType(const Expr *value) const {
  auto *conversion = dyn_cast<CastExpr>(value);
  if (conversion != nullptr && conversion->isImplicit()) {
    return conversion->operand->type;
  }

  return value->type;
}

void NullishAssigns::defineFieldOf(const MemberExpr *member) {
  auto path = this->state.flow.tracked.fieldPath(member);
  if (path.has_value()) {
    this->state.walk.flow.defineField(*path);
  }
}

}
