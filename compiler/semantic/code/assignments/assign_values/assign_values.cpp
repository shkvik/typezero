#include "compiler/semantic/code/assignments/assign_values/assign_values.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr size_t kCompoundOperatorLength = 2;
constexpr const char *kAssignSuffix = "=";

}

AssignValues::AssignValues(SemanticState &owner, AssignmentRules &sharedAssignmentRules)
    : state(owner),
      assignmentRules(sharedAssignmentRules) {}

void AssignValues::afterAssignTarget(const AfterAssignTarget &task) {
  AssignExpr *assign = task.assign;
  const Type *type = this->state.walk.tasks.takeAnswer();
  assign->type = type;
  auto *name = dyn_cast<NameExpr>(assign->target);
  auto place = AssignPlace{
      .index = dyn_cast<IndexExpr>(assign->target),
      .member = dyn_cast<MemberExpr>(assign->target),
      .name = name,
      .variable = name != nullptr ? name->var() : nullptr,
      .form = task.form,
      .hasReportedRange = false,
      .isDefinite = false,
  };
  place.hasReportedRange = this->reportCompoundRange(place);
  if (type == nullptr) {
    this->state.walk.tasks.pushTask(AfterInferredField{.assign = assign, .place = place});
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &assign->value, .expected = nullptr});
  }
  if (task.form == AssignForm::PLAIN) {
    return this->startPlainValue(assign, type, place);
  }

  return this->startCompoundValue(assign, task.op, type, place);
}

void AssignValues::afterInferredField(const AfterInferredField &task) {
  AssignExpr *assign = task.assign;
  auto place = task.place;
  MemberExpr *member = place.member;
  const Expr *value = assign->value;
  auto found = this->checkFieldTypeFrom(member, value->type);
  if (found.isErr()) {
    this->state.diagnostics.error(value->loc(), found.error().message);
  }
  const Type *type = found.isOk() ? value->type : Type::error();
  auto *classDecl = this->classOf(member->object->type);
  for (ClassField &field : classDecl->fields) {
    if (field.name == member->name && !field.isStatic()) {
      field.resolved = type;
    }
  }
  member->type = type;
  assign->type = type;
  place.isDefinite = !type->isOptional() && !type->isUndefined();

  return this->afterAssignValue(AfterAssignValue{.assign = assign, .place = place});
}

void AssignValues::startPlainValue(AssignExpr *assign, const Type *type, const AssignPlace &place) {
  auto *functionExpr = dyn_cast<FunctionExpr>(assign->value);
  if (functionExpr != nullptr && place.name != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = place.name->name;
  }
  const Type *hint = type->isError() ? nullptr : type;
  this->state.walk.tasks.pushTask(AfterPlainValue{.assign = assign, .type = type, .place = place});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &assign->value, .expected = hint});
}

void AssignValues::afterPlainValue(const AfterPlainValue &task) {
  AssignExpr *assign = task.assign;
  const Type *type = task.type;
  auto place = task.place;
  Expr *value = assign->value;
  place.isDefinite = !value->type->isOptional() && !value->type->isUndefined();
  const VarSymbol *variable = place.variable;
  this->state.walk.tasks.pushTask(AfterAssignValue{.assign = assign, .place = place});
  if (variable != nullptr) {
    this->state.walk.tasks.pushTask(NoteAssigned{.variable = variable, .value = value});
  }
  bool isUntypedTarget = variable != nullptr && type->isError() &&
                         this->state.code.variables.hasUntyped() && !value->type->isError() &&
                         !value->type->isVoid();
  if (isUntypedTarget) {
    this->state.code.variables.addUntypedAssignment(variable, value->type);
  }

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = &assign->value, .to = type, .what = ""});
}

void AssignValues::startCompoundValue(AssignExpr *assign, BinaryOp op, const Type *type,
                                      const AssignPlace &place) {
  if (type->isString()) {
    this->state.walk.tasks.pushTask(
        AfterStringCompound{.assign = assign, .op = op, .type = type, .place = place});
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &assign->value, .expected = nullptr});
  }
  this->state.walk.tasks.pushTask(AfterAssignValue{.assign = assign, .place = place});
  if (place.variable != nullptr) {
    this->state.walk.tasks.pushTask(
        CompoundFacts{.assign = assign, .op = op, .variable = place.variable});
  }
  bool isShift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  bool isBitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
  bool isIntegerOnly = isShift || isBitwise;
  bool isDecimalPow = type->isDecimal() && op == BinaryOp::Pow;
  bool isHintless = isDecimalPow || (isIntegerOnly && !type->isInteger());
  bool isUnhinted = isShift && !this->state.code.conversions.isIntLiteralTree(assign->value);
  const Type *hint = isHintless || isUnhinted ? nullptr : type;
  this->state.walk.tasks.pushTask(AfterNumericCompound{.assign = assign, .op = op, .type = type});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &assign->value, .expected = hint});
}

void AssignValues::afterStringCompound(const AfterStringCompound &task) {
  AssignExpr *assign = task.assign;
  this->state.walk.tasks.pushTask(AfterAssignValue{.assign = assign, .place = task.place});
  if (task.place.variable != nullptr) {
    this->state.walk.flow.getFacts().forget(task.place.variable);
  }
  auto spelling = std::string(Operators::opSpelling(task.op)) + kAssignSuffix;
  auto text = this->checkStringAssign(task.op, spelling, task.type, assign);
  if (text.isErr()) {
    return this->state.diagnostics.error(assign->loc(), text.error().message);
  }
  if (task.op == BinaryOp::Add) {
    return this->state.walk.tasks.pushTask(ToStringSlot{.slot = &assign->value});
  }
}

void AssignValues::afterNumericCompound(const AfterNumericCompound &task) {
  AssignExpr *assign = task.assign;
  BinaryOp op = task.op;
  const Type *type = task.type;
  const Type *valueType = assign->value->type;
  bool isShift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  bool isBitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
  bool isIntegerOnly = isShift || isBitwise;
  if (type->isDecimal() && !isIntegerOnly && !valueType->isError()) {
    return this->state.walk.tasks.pushTask(WalkDecimalAssign{.assign = assign, .op = op});
  }
  if (type->isError() || valueType->isError()) {
    return;
  }
  auto spelling = std::string(Operators::opSpelling(op)) + kAssignSuffix;
  bool isAdapting = !isShift && valueType->isInteger() && valueType != type &&
                    this->state.code.conversions.isLiteralAdapting(assign->value, type);
  bool isWrapping = !isShift && type->isWrapping() && valueType->isInteger() &&
                    (!valueType->isWrapping() || valueType == type);
  bool isSettled = isAdapting || isWrapping;
  auto operands = this->checkCompoundOperands(op, spelling, type, assign->value, isSettled);
  if (operands.isErr()) {
    return this->state.diagnostics.error(assign->loc(), operands.error().message);
  }
  if (isAdapting) {
    return this->state.walk.tasks.pushTask(AdaptLiteralSlot{.slot = &assign->value, .to = type});
  }
  if (isWrapping || type->isInteger()) {
    this->state.walk.tasks.pushTask(CheckAssignDivisor{.assign = assign, .op = op});
  }
  if (isWrapping || isShift) {
    return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &assign->value, .to = type});
  }

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = &assign->value, .to = type, .what = ""});
}

void AssignValues::checkAssignDivisor(const CheckAssignDivisor &task) {
  auto divisor = this->checkDivisor(task.op, task.assign->value);
  if (divisor.isErr()) {
    return this->state.diagnostics.error(task.assign->value->loc(), divisor.error().message);
  }
}

AssignValues::ResultDivisorNotZero AssignValues::checkDivisor(BinaryOp op,
                                                              const Expr *divisor) const {
  bool isDividing = op == BinaryOp::Div || op == BinaryOp::Rem;
  if (isDividing && this->state.code.constants.intConstant(divisor) == 0) {
    auto message = ErrorTextDivisor::zero();

    return err(CodeArithmeticRegistryCheckDivisorError{
        .code = CodeArithmeticRegistryCheckDivisorErrorCode::DIVISION_BY_ZERO,
        .message = message,
    });
  }

  return ok(divisor);
}

std::string AssignValues::operandTypesText(llvm::StringRef op, const Type *lhs,
                                           const Type *rhs) const {
  bool isUndefined =
      lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined();

  return ErrorTextOperands::types(op, lhs->name(), rhs->name(), isUndefined);
}

void AssignValues::compoundFacts(const CompoundFacts &task) {
  auto delta = this->state.flow.measurements.linear(task.assign->value);
  bool isStep = (task.op == BinaryOp::Add || task.op == BinaryOp::Sub) && delta.has_value() &&
                delta->term.isZero();
  if (!isStep) {
    return this->state.walk.flow.getFacts().forget(task.variable);
  }
  int64_t step = task.op == BinaryOp::Add ? delta->offset : -delta->offset;

  return this->state.walk.tasks.pushTask(NoteIncremented{.variable = task.variable, .step = step});
}

void AssignValues::afterAssignValue(const AfterAssignValue &task) {
  AssignExpr *assign = task.assign;
  const AssignPlace &place = task.place;
  auto *set = this->recordAssign(assign);
  if (set != nullptr) {
    return this->state.walk.tasks.pushChecked(set);
  }
  this->state.walk.tasks.pushTask(AssignedLength{.assign = assign, .place = place});
  IndexExpr *index = place.index;
  if (index == nullptr) {
    return;
  }
  if (index->object->type->isTypedArray()) {
    return this->state.walk.tasks.pushTask(WalkBytesStore{.index = index, .value = assign->value});
  }
  if (!index->object->type->isArray()) {
    return;
  }
  bool isInRange = this->state.flow.proofs.canProveInBounds(index->object, index->index);
  auto safe = this->checkElementWritable(index, isInRange, place.form);
  if (safe.isErr() && !place.hasReportedRange) {
    this->state.diagnostics.error(index->loc(), safe.error().message);
  }
  ElementIndex *element = index->form.as<ElementIndex>();
  if (element != nullptr) {
    element->mayAppend = !isInRange;
  }
  if (!isInRange) {
    return this->state.walk.tasks.pushTask(
        NoteArrayResized{.array = index->object->type, .isGrowing = true});
  }
}

void AssignValues::assignedLength(const AssignedLength &task) {
  AssignExpr *assign = task.assign;
  MemberExpr *member = task.place.member;
  bool isLength =
      member != nullptr && member->object->type->isArray() && !assign->value->type->isError();
  if (!isLength) {
    return this->assignedField(AssignedField{.assign = assign, .place = task.place});
  }
  this->state.walk.tasks.pushTask(AssignedField{.assign = assign, .place = task.place});
  this->state.walk.tasks.pushTask(LengthFacts{.member = member, .value = assign->value});
  this->state.walk.tasks.pushTask(
      NoteArrayResized{.array = member->object->type, .isGrowing = false});
  const Type *element = member->object->type->inner();
  if (element->hasZeroValue()) {
    auto what = ErrorTextAssign::length();
    return this->state.walk.tasks.pushTask(
        RequireNonNegative{.value = assign->value, .what = what});
  }
  auto length = this->checkLengthProven(member, assign->value);
  if (length.isErr()) {
    return this->state.diagnostics.error(assign->value->loc(), length.error().message);
  }
}

void AssignValues::lengthFacts(const LengthFacts &task) {
  auto self = this->state.flow.measurements.lengthOf(task.member->object);
  if (!self.has_value() || self->term.isZero()) {
    return;
  }
  auto given = this->state.flow.measurements.linear(task.value);
  if (given.has_value()) {
    this->state.walk.flow.getFacts().addEqual(self->term, given->term, given->offset);
  }
}

void AssignValues::assignedField(const AssignedField &task) {
  this->state.walk.tasks.pushChecked(task.assign);
  MemberExpr *member = task.place.member;
  if (member == nullptr || !member->object->type->isObject()) {
    return;
  }
  this->state.walk.flow.forgetFieldsNamed(member->name);
  auto path = this->state.flow.tracked.fieldPath(member);
  if (path.has_value() && task.place.isDefinite) {
    this->state.walk.flow.defineField(*path);
  }
  if (this->isInitializerThis(member->object)) {
    this->state.walk.flow.addSetField(member->name);
  }
}

bool AssignValues::isInitializerThis(const Expr *expr) const {
  const Expr *plain = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(plain);
  const VarSymbol *initializerThis = this->state.walk.self.getInitializerThis();
  bool isInConstructor =
      initializerThis != nullptr &&
      this->state.walk.functions.getCurrentDecl() == this->state.walk.self.getInitializer();

  return isInConstructor && name != nullptr && name->var() == initializerThis;
}

Expr *AssignValues::recordAssign(AssignExpr *assign) {
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  Expr *object = this->targetObject(index, member);
  bool isRecordSet = object != nullptr && object->type != nullptr && object->type->isRecord() &&
                     !assign->op.has_value();
  if (!isRecordSet) {
    return nullptr;
  }
  Expr *key = index != nullptr ? index->index
                               : this->assignmentRules.keyLiteral(member->loc(), member->name);
  auto *setMember = this->state.ast.make<MemberExpr>(assign->loc(), object,
                                                     DictMethodTable::getName(DictMethod::SET));
  const Type *valueType = object->type->valueType();
  setMember->type = valueType;
  auto *call = this->state.ast.make<CallExpr>(assign->loc(), setMember,
                                              std::vector<Expr *>{key, assign->value});
  call->target = BuiltinCall{.builtin = Builtin::RecordSet};
  call->type = valueType;

  return call;
}

Expr *AssignValues::targetObject(const IndexExpr *index, const MemberExpr *member) const {
  if (index != nullptr) {
    return index->object;
  }
  if (member != nullptr) {
    return member->object;
  }

  return nullptr;
}

AssignValues::ResultFieldTypeIsFound AssignValues::checkFieldTypeFrom(const MemberExpr *member,
                                                                      const Type *type) const {
  if (type->isVoid() || type->isUndefined()) {
    auto message = ErrorTextAssign::untyped(member->name);

    return err(CodeAssignmentsRegistryFieldTypeFromError{
        .code = CodeAssignmentsRegistryFieldTypeFromErrorCode::UNTYPED_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

AssignValues::ResultStringAssignable
AssignValues::checkStringAssign(BinaryOp op, llvm::StringRef spelling, const Type *type,
                                const AssignExpr *assign) const {
  const Type *valueType = assign->value->type;
  if (op != BinaryOp::Add && !valueType->isError()) {
    auto message = this->operandTypesText(spelling, type, valueType);

    return err(CodeAssignmentsRegistryStringAssignError{
        .code = CodeAssignmentsRegistryStringAssignErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool isJoining = valueType->nonOptional()->isNumeric() || valueType->nonOptional()->isBool();
  if (op == BinaryOp::Add && isJoining) {
    auto hint = this->joinHint(assign);
    auto message = ErrorTextCompound::joinsText(valueType->name(), hint);

    return err(CodeAssignmentsRegistryStringAssignError{
        .code = CodeAssignmentsRegistryStringAssignErrorCode::JOINS_AS_TEXT,
        .message = message,
    });
  }

  return ok(type);
}

std::string AssignValues::joinHint(const AssignExpr *assign) const {
  auto [target, value] =
      SharedHelpers::operandTexts(assign->source, assign->loc(), kCompoundOperatorLength);
  if (target.empty()) {
    return ErrorTextCompound::convertTo();
  }
  auto shown = SharedHelpers::withoutParens(value);

  return ErrorTextCompound::writeAs(target, shown);
}

AssignValues::ResultCompoundOperands
AssignValues::checkCompoundOperands(BinaryOp op, llvm::StringRef spelling, const Type *type,
                                    const Expr *value, bool isSettled) const {
  const Type *valueType = value->type;
  bool isShift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  bool isBitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
  bool isIntegerOnly = isShift || isBitwise;
  auto *zero = dyn_cast<IntLitExpr>(value);
  if (isIntegerOnly && (type->isFloat() || valueType->isFloat())) {
    auto message = ErrorTextCompound::bitsDouble(spelling);

    return err(CodeAssignmentsRegistryCompoundOperandsError{
        .code = CodeAssignmentsRegistryCompoundOperandsErrorCode::DOUBLE_BITS,
        .message = message,
    });
  }
  bool isWrongType = isIntegerOnly ? !type->isInteger() : !type->isNumeric();
  if (isWrongType) {
    auto message = this->operandTypesText(spelling, type, valueType);

    return err(CodeAssignmentsRegistryCompoundOperandsError{
        .code = CodeAssignmentsRegistryCompoundOperandsErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool isOrZero = (op == BinaryOp::BitOr || op == BinaryOp::UShr) && zero != nullptr &&
                  zero->value == 0 && !type->isWrapping();
  if (isOrZero) {
    auto message = ErrorTextCompound::nothing(spelling);

    return err(CodeAssignmentsRegistryCompoundOperandsError{
        .code = CodeAssignmentsRegistryCompoundOperandsErrorCode::NOTHING_CHANGES,
        .message = message,
    });
  }
  if (op == BinaryOp::UShr && !type->isUnsigned()) {
    auto message = ErrorTextCompound::signedTo(type->name());

    return err(CodeAssignmentsRegistryCompoundOperandsError{
        .code = CodeAssignmentsRegistryCompoundOperandsErrorCode::SIGNED_SHIFT,
        .message = message,
    });
  }
  if (!isSettled && isBitwise && valueType->isInteger() && valueType != type) {
    auto hint = this->state.code.conversions.conversionHint(value, type);
    auto message = ErrorTextCompound::mixed(spelling, type->name(), valueType->name(), hint);

    return err(CodeAssignmentsRegistryCompoundOperandsError{
        .code = CodeAssignmentsRegistryCompoundOperandsErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }

  return ok(type);
}

AssignValues::ResultElementWriteSafe
AssignValues::checkElementWritable(const IndexExpr *index, bool isInRange, AssignForm form) const {
  bool isUnsafe =
      !isInRange && (form == AssignForm::COMPOUND ||
                     !this->state.flow.proofs.canProveInBoundsOrEnd(index->object, index->index));
  if (isUnsafe) {
    auto message = ErrorTextAssign::outOfRange();

    return err(CodeAssignmentsRegistryElementWritableError{
        .code = CodeAssignmentsRegistryElementWritableErrorCode::OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(index);
}

AssignValues::ResultLengthIsInBounds AssignValues::checkLengthProven(const MemberExpr *member,
                                                                     const Expr *value) const {
  bool isProven = this->state.flow.proofs.canProveInBoundsOrEnd(member->object, value) ||
                  this->state.flow.proofs.isExempt(value->loc(), member->object);
  if (!isProven) {
    auto array = BytesCheckHelpers::valueText(member->object);
    auto message = ErrorTextAssign::lengthRange(member->object->type->inner()->name(), array);

    return err(CodeAssignmentsRegistryLengthProvenError{
        .code = CodeAssignmentsRegistryLengthProvenErrorCode::LENGTH_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(value);
}

bool AssignValues::reportCompoundRange(const AssignPlace &place) {
  IndexExpr *index = place.index;
  bool isCompoundElement =
      index != nullptr && place.form == AssignForm::COMPOUND && index->object->type->isArray();
  if (!isCompoundElement) {
    return false;
  }
  auto within = this->assignmentRules.checkIndexProven(index);
  if (within.isOk()) {
    return false;
  }
  this->state.diagnostics.error(index->loc(), within.error().message);

  return true;
}

ClassDecl *AssignValues::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

}
