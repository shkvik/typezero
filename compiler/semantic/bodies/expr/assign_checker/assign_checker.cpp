#include "compiler/semantic/bodies/expr/assign_checker/assign_checker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr size_t kCompoundOperatorLength = 2;
constexpr const char *kAssignSuffix = "=";
constexpr const char *kIncrementSpelling = "++";
constexpr const char *kDecrementSpelling = "--";

}

AssignChecker::AssignChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->fieldFacts = &all.fieldFacts;
  this->varChecker = &all.varChecker;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->indexProofs = &all.indexProofs;
  this->bytesChecker = &all.bytesChecker;
  this->classChecker = &all.classChecker;
  this->truthChecker = &all.truthChecker;
  this->binaryChecker = &all.binaryChecker;
  this->concatChecker = &all.concatChecker;
  this->targetChecker = &all.targetChecker;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->constantFolder = &all.constantFolder;
  this->decimalChecker = &all.decimalChecker;
  this->genericChecker = &all.genericChecker;
  this->processChecker = &all.processChecker;
  this->topLevelChecker = &all.topLevelChecker;
  this->builtinModuleChecker = &all.builtinModuleChecker;
}

Expr *AssignChecker::copyChain(const Expr *expr) {
  auto members = std::vector<const MemberExpr *>();
  const Expr *link = expr;
  auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    if (member->optional) {
      return nullptr;
    }
    members.push_back(member);
    link = member->object;
    member = dyn_cast<MemberExpr>(link);
  }
  auto *copy = this->copyRoot(link);
  if (copy == nullptr) {
    return nullptr;
  }
  for (const MemberExpr *step : llvm::reverse(members)) {
    copy = this->state.getAst().make<MemberExpr>(step->loc(), copy, step->name);
  }

  return copy;
}

Expr *AssignChecker::copyRoot(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *integer = dyn_cast<IntLitExpr>(expr);
  auto *text = dyn_cast<StringLitExpr>(expr);
  if (name != nullptr) {
    return this->state.getAst().make<NameExpr>(name->loc(), name->name);
  }
  if (integer != nullptr) {
    return this->state.getAst().make<IntLitExpr>(integer->loc(), integer->value);
  }
  if (text != nullptr) {
    return this->state.getAst().make<StringLitExpr>(text->loc(), text->value);
  }

  return nullptr;
}

AssignChecker::ResultNameIsAssignable AssignChecker::checkNotImported(const NameExpr *name) {
  if (this->builtinModuleChecker->nodeName(name->name) != nullptr) {
    auto message = ErrorTextAssign::imported(name->name);

    return err(AssignCheckerNotImportedError{
        .code = AssignCheckerNotImportedErrorCode::IMPORTED,
        .message = message,
    });
  }

  return ok(name);
}

AssignChecker::ResultTargetIsVariable AssignChecker::checkVariableTarget(const Expr *target) const {
  if (!isa<NameExpr>(target)) {
    auto message = ErrorTextAssign::nullishVar();

    return err(AssignCheckerVariableTargetError{
        .code = AssignCheckerVariableTargetErrorCode::NOT_A_VARIABLE,
        .message = message,
    });
  }

  return ok(target);
}

AssignChecker::ResultVariableOptional AssignChecker::checkNullishOptional(const NameExpr *name,
                                                                          const Type *type) const {
  if (!type->isOptional() && !type->isError()) {
    auto message = ErrorTextAssign::notOptional(name->name, type->name());

    return err(AssignCheckerNullishOptionalError{
        .code = AssignCheckerNullishOptionalErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(type);
}

AssignChecker::ResultIndexWithinArray
AssignChecker::checkIndexProven(const IndexExpr *index) const {
  if (!this->indexProofs->canProveInBounds(index->object, index->index)) {
    auto message = ErrorTextAssign::indexRange();

    return err(AssignCheckerIndexProvenError{
        .code = AssignCheckerIndexProvenErrorCode::INDEX_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(index);
}

AssignChecker::ResultFieldTypeIsFound AssignChecker::checkFieldTypeFrom(const MemberExpr *member,
                                                                        const Type *type) const {
  if (type->isVoid() || type->isUndefined()) {
    auto message = ErrorTextAssign::untyped(member->name);

    return err(AssignCheckerFieldTypeFromError{
        .code = AssignCheckerFieldTypeFromErrorCode::UNTYPED_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

AssignChecker::ResultStringAssignable
AssignChecker::checkStringAssign(BinaryOp op, llvm::StringRef spelling, const Type *type,
                                 const AssignExpr *assign) const {
  const Type *valueType = assign->value->type;
  if (op != BinaryOp::Add && !valueType->isError()) {
    auto message = this->binaryChecker->operandTypesText(spelling, type, valueType);

    return err(AssignCheckerStringAssignError{
        .code = AssignCheckerStringAssignErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool isJoining = valueType->nonOptional()->isNumeric() || valueType->nonOptional()->isBool();
  if (op == BinaryOp::Add && isJoining) {
    auto hint = this->joinHint(assign);
    auto message = ErrorTextCompound::joinsText(valueType->name(), hint);

    return err(AssignCheckerStringAssignError{
        .code = AssignCheckerStringAssignErrorCode::JOINS_AS_TEXT,
        .message = message,
    });
  }

  return ok(type);
}

std::string AssignChecker::joinHint(const AssignExpr *assign) const {
  auto [target, value] =
      SharedHelpers::operandTexts(assign->source, assign->loc(), kCompoundOperatorLength);
  if (target.empty()) {
    return ErrorTextCompound::convertTo();
  }
  auto shown = SharedHelpers::withoutParens(value);

  return ErrorTextCompound::writeAs(target, shown);
}

AssignChecker::ResultCompoundOperands
AssignChecker::checkCompoundOperands(BinaryOp op, llvm::StringRef spelling, const Type *type,
                                     const Expr *value, bool isSettled) const {
  const Type *valueType = value->type;
  bool isShift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  bool isBitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
  bool isIntegerOnly = isShift || isBitwise;
  auto *zero = dyn_cast<IntLitExpr>(value);
  if (isIntegerOnly && (type->isFloat() || valueType->isFloat())) {
    auto message = ErrorTextCompound::bitsDouble(spelling);

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::DOUBLE_BITS,
        .message = message,
    });
  }
  bool isWrongType = isIntegerOnly ? !type->isInteger() : !type->isNumeric();
  if (isWrongType) {
    auto message = this->binaryChecker->operandTypesText(spelling, type, valueType);

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool isOrZero = (op == BinaryOp::BitOr || op == BinaryOp::UShr) && zero != nullptr &&
                  zero->value == 0 && !type->isWrapping();
  if (isOrZero) {
    auto message = ErrorTextCompound::nothing(spelling);

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::NOTHING_CHANGES,
        .message = message,
    });
  }
  if (op == BinaryOp::UShr && !type->isUnsigned()) {
    auto message = ErrorTextCompound::signedTo(type->name());

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::SIGNED_SHIFT,
        .message = message,
    });
  }
  if (!isSettled && isBitwise && valueType->isInteger() && valueType != type) {
    auto hint = this->typeRelations->conversionHint(value, type);
    auto message = ErrorTextCompound::mixed(spelling, type->name(), valueType->name(), hint);

    return err(AssignCheckerCompoundOperandsError{
        .code = AssignCheckerCompoundOperandsErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }

  return ok(type);
}

AssignChecker::ResultElementWriteSafe
AssignChecker::checkElementWritable(const IndexExpr *index, bool isInRange, AssignForm form) const {
  bool isUnsafe =
      !isInRange && (form == AssignForm::COMPOUND ||
                     !this->indexProofs->canProveInBoundsOrEnd(index->object, index->index));
  if (isUnsafe) {
    auto message = ErrorTextAssign::outOfRange();

    return err(AssignCheckerElementWritableError{
        .code = AssignCheckerElementWritableErrorCode::OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(index);
}

AssignChecker::ResultLengthIsInBounds AssignChecker::checkLengthProven(const MemberExpr *member,
                                                                       const Expr *value) const {
  bool isProven = this->indexProofs->canProveInBoundsOrEnd(member->object, value) ||
                  this->indexProofs->isExempt(value->loc(), member->object);
  if (!isProven) {
    auto array = BytesCheckHelpers::valueText(member->object);
    auto message = ErrorTextAssign::lengthRange(member->object->type->inner()->name(), array);

    return err(AssignCheckerLengthProvenError{
        .code = AssignCheckerLengthProvenErrorCode::LENGTH_NOT_PROVEN,
        .message = message,
    });
  }

  return ok(value);
}

Expr *AssignChecker::checkAssign(AssignExpr *assign) {
  this->walkTargetAlias(assign);
  auto *imported = this->reportImportedTarget(assign);
  if (imported != nullptr) {
    return imported;
  }
  Expr *exitCode = this->processChecker->checkProcessAssign(assign);
  if (exitCode != nullptr) {
    return exitCode;
  }
  bool isCompound = assign->op.has_value();
  BinaryOp op = isCompound ? *assign->op : BinaryOp::Add;
  if (isCompound && (op == BinaryOp::LogOr || op == BinaryOp::LogAnd)) {
    return this->checkLogicalAssignValue(assign);
  }
  if (isCompound && op == BinaryOp::Nullish) {
    return this->checkNullishAssign(assign);
  }
  bool isUnfolded = isCompound && this->unfoldNarrowedCompound(assign, op);
  bool isStillCompound = isCompound && !isUnfolded;
  auto form = isStillCompound ? AssignForm::COMPOUND : AssignForm::PLAIN;

  return this->checkPlainAssign(assign, op, form);
}

void AssignChecker::walkTargetAlias(AssignExpr *assign) {
  auto *member = dyn_cast<MemberExpr>(assign->target);
  auto *index = dyn_cast<IndexExpr>(assign->target);
  if (member != nullptr) {
    member->object = this->builtinModuleChecker->processAlias(member->object);
    return;
  }
  if (index != nullptr) {
    index->object = this->builtinModuleChecker->processAlias(index->object);
  }
}

Expr *AssignChecker::reportImportedTarget(AssignExpr *assign) {
  auto *name = dyn_cast<NameExpr>(assign->target);
  if (name == nullptr) {
    return nullptr;
  }
  auto assignable = this->checkNotImported(name);
  if (assignable.isOk()) {
    return nullptr;
  }
  this->state.getDiagnostics().error(name->loc(), assignable.error().message);
  assign->value = this->exprChecker->checkExpr(assign->value);
  assign->type = Type::error();

  return assign;
}

Expr *AssignChecker::checkLogicalAssignValue(AssignExpr *assign) {
  auto logical = this->checkLogicalAssign(assign);
  if (logical.isErr()) {
    this->state.getDiagnostics().error(assign->target->loc(), logical.error().message);
    return assign;
  }

  return logical.value();
}

Expr *AssignChecker::checkNullishAssign(AssignExpr *assign) {
  auto *record = this->checkRecordFieldNullish(assign);
  if (record != nullptr) {
    return record;
  }
  if (isa<MemberExpr>(assign->target)) {
    return this->checkFieldLogicalAssign(assign);
  }
  auto *index = dyn_cast<IndexExpr>(assign->target);
  if (index == nullptr) {
    return this->checkVariableNullish(assign);
  }
  auto *set = this->dictChecker->checkRecordNullishAssign(assign, index);
  if (set != nullptr) {
    return set;
  }

  return this->checkVariableNullish(assign);
}

Expr *AssignChecker::checkRecordFieldNullish(AssignExpr *assign) {
  auto *member = dyn_cast<MemberExpr>(assign->target);
  if (member == nullptr || member->optional) {
    return nullptr;
  }
  if (!this->isRecordHolder(member->object)) {
    return nullptr;
  }
  auto *key = this->dictChecker->keyLiteral(member->loc(), member->name);
  auto *index = this->state.getAst().make<IndexExpr>(member->loc(), member->object, key);
  assign->target = index;

  return this->dictChecker->checkRecordNullishAssign(assign, index);
}

bool AssignChecker::isRecordHolder(const Expr *object) const {
  auto *name = dyn_cast<NameExpr>(object);
  VarSymbol *variable =
      name != nullptr ? this->topLevelChecker->lookupValue(name->name).var() : nullptr;
  bool isRecordVariable =
      variable != nullptr && variable->type != nullptr && variable->type->isRecord();

  return isRecordVariable || this->processChecker->isProcessMember(object, ProcessMember::ENV);
}

Expr *AssignChecker::checkVariableNullish(AssignExpr *assign) {
  auto *name = dyn_cast<NameExpr>(assign->target);
  VarSymbol *variable = name != nullptr ? this->targetChecker->checkVarTarget(name) : nullptr;
  auto target = this->checkVariableTarget(assign->target);
  if (target.isErr()) {
    this->state.getDiagnostics().error(assign->target->loc(), target.error().message);
  }
  const Type *declared = variable != nullptr ? variable->type : Type::error();
  assign->type = declared;
  auto optional = this->checkNullishOptional(name, declared);
  if (optional.isErr()) {
    this->state.getDiagnostics().error(assign->loc(), optional.error().message);
  }
  const Type *type = optional.isOk() ? declared : Type::error();
  this->walkNullishValue(assign, type);
  if (type->isError()) {
    return assign;
  }
  const Type *valueType = assign->value->type;
  bool isDefinite = !valueType->isOptional() && !valueType->isUndefined();
  const Type *stored = isDefinite ? type->inner() : type;
  assign->value = this->typeRelations->coerce(assign->value, stored);
  if (variable == nullptr) {
    return assign;
  }
  if (isDefinite) {
    this->flowChecker->markDefined(variable);
  }
  assign->type = stored;

  return assign;
}

void AssignChecker::walkNullishValue(AssignExpr *assign, const Type *type) {
  auto before = this->state.getFlowState().getFlow();
  const Type *hint = type->isError() ? nullptr : type->inner();
  assign->value = this->exprChecker->checkExpr(assign->value, hint);
  const auto &after = this->state.getFlowState().getFlow();
  auto joined = this->flowChecker->join(before, after);
  this->state.getFlowState().replaceFlow(joined);
}

bool AssignChecker::unfoldNarrowedCompound(AssignExpr *assign, BinaryOp op) {
  auto *target = dyn_cast<NameExpr>(assign->target);
  if (target == nullptr) {
    return false;
  }
  const VarSymbol *variable = this->scopes->lookup(target->name).var();
  bool isNarrowed = variable != nullptr && variable->type != nullptr &&
                    variable->type->nonOptional()->isUnion() &&
                    this->genericChecker->narrowedType(variable) != nullptr;
  if (!isNarrowed) {
    return false;
  }
  auto *read = this->state.getAst().make<NameExpr>(target->loc(), target->name);
  assign->value = this->state.getAst().make<BinaryExpr>(assign->loc(), op, read, assign->value);
  assign->op.reset();

  return true;
}

Expr *AssignChecker::checkPlainAssign(AssignExpr *assign, BinaryOp op, AssignForm form) {
  const auto *type = this->targetChecker->checkAssignTarget(assign->target, form);
  assign->type = type;
  auto *name = dyn_cast<NameExpr>(assign->target);
  auto place = AssignPlace{
      .index = dyn_cast<IndexExpr>(assign->target),
      .member = dyn_cast<MemberExpr>(assign->target),
      .name = name,
      .variable = name != nullptr ? name->var() : nullptr,
      .form = form,
      .hasReportedRange = false,
      .isDefinite = false,
  };
  place.hasReportedRange = this->reportCompoundRange(place);
  if (type == nullptr) {
    this->walkInferredField(assign, place);
  } else if (form == AssignForm::PLAIN) {
    this->walkPlainValue(assign, type, place);
  } else {
    this->walkCompoundValue(assign, op, type, place);
  }
  auto *set = this->dictChecker->recordAssign(assign);
  if (set != nullptr) {
    return set;
  }
  this->walkAssignedElement(assign, place);
  this->walkAssignedLength(assign, place);
  this->walkAssignedField(place);

  return assign;
}

bool AssignChecker::reportCompoundRange(const AssignPlace &place) {
  IndexExpr *index = place.index;
  bool isCompoundElement =
      index != nullptr && place.form == AssignForm::COMPOUND && index->object->type->isArray();
  if (!isCompoundElement) {
    return false;
  }
  auto within = this->checkIndexProven(index);
  if (within.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(index->loc(), within.error().message);

  return true;
}

void AssignChecker::walkInferredField(AssignExpr *assign, AssignPlace &place) {
  MemberExpr *member = place.member;
  auto *value = this->exprChecker->checkExpr(assign->value);
  auto found = this->checkFieldTypeFrom(member, value->type);
  if (found.isErr()) {
    this->state.getDiagnostics().error(value->loc(), found.error().message);
  }
  const Type *type = found.isOk() ? value->type : Type::error();
  auto *classDecl = this->classDeclarer->classOf(member->object->type);
  for (ClassField &field : classDecl->fields) {
    if (field.name == member->name && !field.isStatic()) {
      field.resolved = type;
    }
  }
  assign->value = value;
  member->type = type;
  assign->type = type;
  place.isDefinite = !type->isOptional() && !type->isUndefined();
}

void AssignChecker::walkPlainValue(AssignExpr *assign, const Type *type, AssignPlace &place) {
  auto *functionExpr = dyn_cast<FunctionExpr>(assign->value);
  if (functionExpr != nullptr && place.name != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = place.name->name;
  }
  const Type *hint = type->isError() ? nullptr : type;
  auto *value = this->exprChecker->checkExpr(assign->value, hint);
  assign->value = this->typeRelations->coerce(value, type);
  place.isDefinite = !value->type->isOptional() && !value->type->isUndefined();
  if (place.variable == nullptr) {
    return;
  }
  this->flowChecker->assigned(place.variable, value);
  if (type->isError() && this->state.getExpressionMarks().hasUntyped()) {
    this->varChecker->assignedUntyped(place.variable, value->type);
  }
}

void AssignChecker::walkCompoundValue(AssignExpr *assign, BinaryOp op, const Type *type,
                                      const AssignPlace &place) {
  auto spelling = std::string(Operators::opSpelling(op)) + kAssignSuffix;
  if (type->isString()) {
    return this->walkStringCompound(assign, op, spelling, type, place.variable);
  }
  this->walkNumericCompound(assign, op, spelling, type);
  if (place.variable != nullptr) {
    this->walkCompoundFacts(assign, op, place.variable);
  }
}

void AssignChecker::walkStringCompound(AssignExpr *assign, BinaryOp op, const std::string &spelling,
                                       const Type *type, const VarSymbol *variable) {
  assign->value = this->exprChecker->checkExpr(assign->value);
  auto text = this->checkStringAssign(op, spelling, type, assign);
  if (text.isErr()) {
    this->state.getDiagnostics().error(assign->loc(), text.error().message);
  } else if (op == BinaryOp::Add) {
    assign->value = this->concatChecker->toStringOperand(assign->value);
  }
  if (variable != nullptr) {
    this->state.getFlowState().getFacts().forget(variable);
  }
}

void AssignChecker::walkNumericCompound(AssignExpr *assign, BinaryOp op,
                                        const std::string &spelling, const Type *type) {
  bool isShift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  bool isBitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
  bool isIntegerOnly = isShift || isBitwise;
  bool isDecimalPow = type->isDecimal() && op == BinaryOp::Pow;
  bool isHintless = isDecimalPow || (isIntegerOnly && !type->isInteger());
  bool isUnhinted = isShift && !this->typeRelations->isIntLiteralTree(assign->value);
  const Type *hint = isHintless || isUnhinted ? nullptr : type;
  assign->value = this->exprChecker->checkExpr(assign->value, hint);
  const Type *valueType = assign->value->type;
  if (type->isDecimal() && !isIntegerOnly && !valueType->isError()) {
    return this->decimalChecker->walkDecimalAssign(assign, op);
  }
  if (type->isError() || valueType->isError()) {
    return;
  }

  return this->walkCompoundOperands(assign, op, spelling, type, isShift);
}

void AssignChecker::walkCompoundOperands(AssignExpr *assign, BinaryOp op,
                                         const std::string &spelling, const Type *type,
                                         bool isShift) {
  const Type *valueType = assign->value->type;
  bool isAdapting = !isShift && valueType->isInteger() && valueType != type &&
                    this->typeRelations->isLiteralAdapting(assign->value, type);
  bool isWrapping = !isShift && type->isWrapping() && valueType->isInteger() &&
                    (!valueType->isWrapping() || valueType == type);
  bool isSettled = isAdapting || isWrapping;
  auto operands = this->checkCompoundOperands(op, spelling, type, assign->value, isSettled);
  if (operands.isErr()) {
    this->state.getDiagnostics().error(assign->loc(), operands.error().message);
    return;
  }
  if (isAdapting) {
    assign->value = this->typeRelations->adaptLiteral(assign->value, type);
    return;
  }
  if (isWrapping) {
    assign->value = this->typeRelations->convert(assign->value, type);
    return this->walkDivisor(assign, op);
  }
  if (isShift) {
    assign->value = this->typeRelations->convert(assign->value, type);
  } else {
    assign->value = this->typeRelations->coerce(assign->value, type);
  }
  if (type->isInteger()) {
    this->walkDivisor(assign, op);
  }
}

void AssignChecker::walkDivisor(AssignExpr *assign, BinaryOp op) {
  auto divisor = this->constantFolder->checkDivisor(op, assign->value);
  if (divisor.isErr()) {
    this->state.getDiagnostics().error(assign->value->loc(), divisor.error().message);
  }
}

void AssignChecker::walkCompoundFacts(AssignExpr *assign, BinaryOp op, const VarSymbol *variable) {
  auto delta = this->indexProofs->linear(assign->value);
  bool isStep =
      (op == BinaryOp::Add || op == BinaryOp::Sub) && delta.has_value() && delta->term.isZero();
  if (!isStep) {
    this->state.getFlowState().getFacts().forget(variable);
    return;
  }
  int64_t step = op == BinaryOp::Add ? delta->offset : -delta->offset;
  this->flowChecker->incremented(variable, step);
}

void AssignChecker::walkAssignedElement(AssignExpr *assign, const AssignPlace &place) {
  IndexExpr *index = place.index;
  if (index == nullptr) {
    return;
  }
  if (index->object->type->isTypedArray()) {
    this->bytesChecker->walkBytesStore(index, assign->value);
  }
  if (!index->object->type->isArray()) {
    return;
  }
  bool isInRange = this->indexProofs->canProveInBounds(index->object, index->index);
  auto safe = this->checkElementWritable(index, isInRange, place.form);
  if (safe.isErr() && !place.hasReportedRange) {
    this->state.getDiagnostics().error(index->loc(), safe.error().message);
  }
  ElementIndex *element = index->form.as<ElementIndex>();
  if (element != nullptr) {
    element->mayAppend = !isInRange;
  }
  if (!isInRange) {
    this->flowChecker->arrayResized(index->object->type, Resize::GROWS);
  }
}

void AssignChecker::walkAssignedLength(AssignExpr *assign, const AssignPlace &place) {
  MemberExpr *member = place.member;
  bool isLength =
      member != nullptr && member->object->type->isArray() && !assign->value->type->isError();
  if (!isLength) {
    return;
  }
  this->walkLengthValue(member, assign->value);
  this->flowChecker->arrayResized(member->object->type, Resize::CHANGES);
  auto self = this->indexProofs->lengthOf(member->object);
  if (!self.has_value() || self->term.isZero()) {
    return;
  }
  auto given = this->indexProofs->linear(assign->value);
  if (given.has_value()) {
    this->state.getFlowState().getFacts().addEqual(self->term, given->term, given->offset);
  }
}

void AssignChecker::walkLengthValue(const MemberExpr *member, const Expr *value) {
  const Type *element = member->object->type->inner();
  if (!element->hasZeroValue()) {
    return this->walkFilledLength(member, value);
  }
  auto what = ErrorTextAssign::length();
  auto between = this->indexProofs->requireBetween(value, 0, std::nullopt, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(value->loc(), between.error().message);
  }
}

void AssignChecker::walkFilledLength(const MemberExpr *member, const Expr *value) {
  auto length = this->checkLengthProven(member, value);
  if (length.isErr()) {
    this->state.getDiagnostics().error(value->loc(), length.error().message);
  }
}

void AssignChecker::walkAssignedField(const AssignPlace &place) {
  MemberExpr *member = place.member;
  if (member == nullptr || !member->object->type->isObject()) {
    return;
  }
  this->fieldFacts->fieldsAssigned(member->name);
  auto path = this->fieldFacts->fieldPath(member);
  if (path.has_value() && place.isDefinite) {
    this->state.getFlowState().defineField(*path);
  }
  if (this->classChecker->isInitThis(member->object).isOk()) {
    this->state.getFlowState().addSetField(member->name);
  }
}

AssignChecker::ResultLogicalTargetSet AssignChecker::checkLogicalAssign(AssignExpr *assign) {
  auto *read = this->logicalRead(assign->target);
  if (read == nullptr) {
    assign->value = this->exprChecker->checkExpr(assign->value);
    assign->type = Type::error();
    auto spelling = std::string(Operators::opSpelling(*assign->op)) + kAssignSuffix;
    auto message = ErrorTextLogicalAssign::simple(spelling);

    return err(AssignCheckerCheckLogicalAssignError{
        .code = AssignCheckerCheckLogicalAssignErrorCode::NOT_A_SIMPLE_TARGET,
        .message = message,
    });
  }
  auto *set = this->state.getAst().make<AssignExpr>(assign->loc(), std::nullopt, assign->target,
                                                    assign->value);
  auto *binary = this->state.getAst().make<BinaryExpr>(assign->loc(), *assign->op, read, set);
  auto whenTrue = Flow();
  auto whenFalse = Flow();
  bool isAnd = binary->op == BinaryOp::LogAnd;
  auto use = isAnd ? TruthUse::AndAssign : TruthUse::OrAssign;
  this->truthChecker->checkLogical(binary, nullptr, whenTrue, whenFalse, RightSide::VALUE, use);
  auto joined = this->flowChecker->join(whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(joined);

  return ok(binary);
}

Expr *AssignChecker::logicalRead(const Expr *target) {
  auto *index = dyn_cast<IndexExpr>(target);
  if (isa<NameExpr>(target) || isa<MemberExpr>(target)) {
    return this->copyChain(target);
  }
  if (index != nullptr && !index->optional) {
    return this->indexRead(index);
  }

  return nullptr;
}

Expr *AssignChecker::indexRead(const IndexExpr *index) {
  auto *object = this->copyChain(index->object);
  auto *position = this->copyChain(index->index);
  if (object == nullptr || position == nullptr) {
    return nullptr;
  }

  return this->state.getAst().make<IndexExpr>(index->loc(), object, position);
}

AssignChecker::ResultFieldChainCopied AssignChecker::checkFieldChain(Expr *read) const {
  if (read == nullptr) {
    auto message = ErrorTextLogicalAssign::field();

    return err(AssignCheckerFieldChainError{
        .code = AssignCheckerFieldChainErrorCode::NOT_A_FIELD_CHAIN,
        .message = message,
    });
  }

  return ok(read);
}

AssignChecker::ResultFieldMayBeAbsent AssignChecker::checkFieldOptional(const MemberExpr *member,
                                                                        const Type *type) const {
  if (type != nullptr && !type->isOptional() && !type->isError()) {
    auto message = ErrorTextLogicalAssign::fieldOpt(member->name, type->name());

    return err(AssignCheckerFieldOptionalError{
        .code = AssignCheckerFieldOptionalErrorCode::NOT_OPTIONAL,
        .message = message,
    });
  }

  return ok(type);
}

Expr *AssignChecker::checkFieldLogicalAssign(AssignExpr *assign) {
  auto *copied = this->copyChain(assign->target);
  auto chain = this->checkFieldChain(copied);
  if (chain.isErr()) {
    this->state.getDiagnostics().error(assign->target->loc(), chain.error().message);
    assign->value = this->exprChecker->checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  Expr *read = chain.value();
  auto *set = this->state.getAst().make<AssignExpr>(assign->loc(), std::nullopt, assign->target,
                                                    assign->value);
  auto *binary = this->state.getAst().make<BinaryExpr>(assign->loc(), BinaryOp::Nullish, read, set);
  auto nullish = this->binaryChecker->checkNullish(binary, nullptr);
  if (nullish.isErr()) {
    this->state.getDiagnostics().error(binary->loc(), nullish.error().message);
  }
  Expr *result = nullish.isOk() ? nullish.value() : binary;
  auto *member = cast<MemberExpr>(set->target);
  auto absent = this->checkFieldOptional(member, member->type);
  if (absent.isErr()) {
    this->state.getDiagnostics().error(assign->loc(), absent.error().message);
    return result;
  }
  const auto *valueType = this->assignedValueType(set->value);
  if (!valueType->isOptional() && !valueType->isUndefined()) {
    this->defineFieldOf(member);
  }

  return result;
}

const Type *AssignChecker::assignedValueType(const Expr *value) const {
  auto *conversion = dyn_cast<CastExpr>(value);
  if (conversion != nullptr && conversion->isImplicit()) {
    return conversion->operand->type;
  }

  return value->type;
}

void AssignChecker::defineFieldOf(const MemberExpr *member) {
  auto path = this->fieldFacts->fieldPath(member);
  if (path.has_value()) {
    this->state.getFlowState().defineField(*path);
  }
}

AssignChecker::ResultIncrementedValue AssignChecker::checkUpdatable(const UpdateExpr *update,
                                                                    const Type *type) const {
  if (!type->isNumeric() && !type->isDecimal()) {
    const char *op = update->increment ? kIncrementSpelling : kDecrementSpelling;
    auto message = ErrorTextUpdate::notNumber(op, type->name());

    return err(AssignCheckerUpdatableError{
        .code = AssignCheckerUpdatableErrorCode::NOT_NUMERIC,
        .message = message,
    });
  }

  return ok(type);
}

Expr *AssignChecker::checkUpdate(UpdateExpr *update) {
  update->type = Type::error();
  const auto *type = this->targetChecker->checkAssignTarget(update->target, AssignForm::COMPOUND);
  if (type->isError()) {
    return update;
  }
  auto numeric = this->checkUpdatable(update, type);
  if (numeric.isErr()) {
    this->state.getDiagnostics().error(update->loc(), numeric.error().message);
    return update;
  }
  update->type = type;
  auto *name = dyn_cast<NameExpr>(update->target);
  auto *index = dyn_cast<IndexExpr>(update->target);
  auto *member = dyn_cast<MemberExpr>(update->target);
  if (name != nullptr) {
    this->walkUpdatedName(name, update);
    return update;
  }
  if (index != nullptr && !index->keyed()) {
    this->walkUpdatedElement(index);
    return update;
  }
  if (member != nullptr) {
    this->fieldFacts->fieldsAssigned(member->name);
  }

  return update;
}

void AssignChecker::walkUpdatedName(const NameExpr *name, const UpdateExpr *update) {
  if (name->var() == nullptr) {
    return;
  }
  int64_t step = update->increment ? 1 : -1;
  this->flowChecker->incremented(name->var(), step);
}

void AssignChecker::walkUpdatedElement(IndexExpr *index) {
  if (index->object->type->isTypedArray()) {
    this->bytesChecker->walkBytesStore(index, nullptr);
    return;
  }
  auto within = this->checkIndexProven(index);
  if (within.isErr()) {
    this->state.getDiagnostics().error(index->loc(), within.error().message);
  }
}

}
