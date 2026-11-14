#include "compiler/semantic/code/indexing/code_indexing_registry.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/number/number_static_table/number_static_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/code/unknown_values/code_unknown_values_registry_errors.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cmath>
#include <cstdio>
#include <string>

using llvm::dyn_cast;

namespace tz {

CodeIndexingRegistry::CodeIndexingRegistry(SemanticState &owner)
    : state(owner) {}

void CodeIndexingRegistry::checkPosition(const CheckPosition &task) {
  this->state.walk.tasks.pushTask(FinishPosition{.slot = task.slot});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = task.slot});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = task.slot, .expected = Type::intTy()});
}

void CodeIndexingRegistry::finishPosition(const FinishPosition &task) {
  const Expr *position = *task.slot;
  const Type *type = position->type;
  if (type->isError()) {
    return;
  }
  auto integer = this->checkPositionType(type);
  if (integer.isErr()) {
    return this->state.diagnostics.error(position->loc(), integer.error().message);
  }

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::intTy()});
}

void CodeIndexingRegistry::checkIndex(const CheckIndex &task) {
  this->state.walk.tasks.pushTask(AfterIndexListPart{.index = task.index});

  return this->state.walk.tasks.pushTask(FindListPart{.expr = task.index});
}

void CodeIndexingRegistry::afterIndexListPart(const AfterIndexListPart &task) {
  IndexExpr *index = task.index;
  Expr *part = this->state.walk.tasks.takeChecked();
  if (part != nullptr) {
    return this->state.walk.tasks.pushChecked(part);
  }
  auto *name = dyn_cast<NameExpr>(index->object);
  EnumDecl *enumDecl =
      name != nullptr ? this->state.walk.scopes.findName(name->name).enumDecl() : nullptr;
  if (enumDecl != nullptr) {
    index->type = Type::error();
    this->state.walk.tasks.pushTask(AfterEnumIndexResolved{.index = index, .enumDecl = enumDecl});
    return this->state.walk.tasks.pushTask(ResolveEnum{.enumDecl = enumDecl});
  }
  this->state.walk.tasks.pushTask(AfterProcessEnvIndex{.index = index});
  this->state.walk.tasks.pushTask(CheckProcessEnvIndex{.index = index});

  return this->state.walk.tasks.pushTask(ProcessAliasSlot{.slot = &index->object});
}

void CodeIndexingRegistry::afterProcessEnvIndex(const AfterProcessEnvIndex &task) {
  IndexExpr *index = task.index;
  Expr *variable = this->state.walk.tasks.takeChecked();
  if (variable != nullptr) {
    return this->state.walk.tasks.pushChecked(variable);
  }
  this->state.walk.tasks.pushTask(AfterIndexObject{.index = index});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &index->object});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &index->object, .expected = nullptr});
}

void CodeIndexingRegistry::afterIndexObject(const AfterIndexObject &task) {
  IndexExpr *index = task.index;
  if (index->optional) {
    index->object = this->chainLink(index->object);
  }
  index->type = Type::error();
  if (this->isGenericIndex(index)) {
    this->state.walk.tasks.pushTask(FinishGenericIndex{.index = index});
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &index->index, .expected = nullptr});
  }
  const Type *type = index->object->type;
  auto &tasks = this->state.walk.tasks;
  if (type->isMatch()) {
    return tasks.pushTask(CheckMatchIndex{.index = index});
  }
  if (type->nonOptional()->isJson()) {
    return tasks.pushTask(CheckJsonIndex{.index = index});
  }
  if (type->isBytes()) {
    return tasks.pushTask(CheckBytesIndex{.index = index});
  }
  if (type->isUnknown() || type->isAnyObject()) {
    tasks.pushTask(FinishUnknownIndex{.index = index});
    return tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
  }
  if (type->isObject()) {
    return tasks.pushTask(CheckKeyedField{.index = index, .access = KeyedAccess::READ});
  }
  if (type->isDict()) {
    return tasks.pushTask(CheckDictIndex{.index = index});
  }
  if (type->isOptional() && type->inner()->isObject()) {
    tasks.pushTask(ReportUndefinedIndex{.index = index});
    return tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
  }
  tasks.pushTask(FinishPositionIndex{.index = index});

  return tasks.pushTask(CheckPosition{.slot = &index->index});
}

bool CodeIndexingRegistry::isGenericIndex(const IndexExpr *index) const {
  const Type *object = index->object->type;
  bool isOpenObject = object->isParam() && !object->isListParam() && object->iterated() == nullptr;
  if (isOpenObject) {
    return true;
  }
  const auto *keyName = dyn_cast<NameExpr>(index->index);

  return object->isObject() && keyName != nullptr && this->isGenericKeyName(keyName);
}

void CodeIndexingRegistry::finishGenericIndex(const FinishGenericIndex &task) {
  IndexExpr *index = task.index;
  const Type *object = index->object->type;
  const Type *key = index->index->type;
  if (key->isError()) {
    return this->state.walk.tasks.pushChecked(index);
  }
  if (!key->isStringLiteral() && !this->isGenericKey(key)) {
    auto message = ErrorTextGenericIndex::notByKey(object->name(), key->name());
    return this->reportIndex(index, CodeIndexingRegistryCheckIndexErrorCode::GENERIC_INDEX,
                             message);
  }
  const auto *indexed = Type::indexedAccess(object, key);
  this->state.walk.tasks.pushTask(WriteIndexType{.index = index});

  return this->state.walk.tasks.pushTask(EvalTypeOp{.op = indexed, .operands = {object, key}});
}

void CodeIndexingRegistry::writeIndexType(const WriteIndexType &task) {
  task.index->type = this->state.walk.tasks.takeAnswer();

  return this->state.walk.tasks.pushChecked(task.index);
}

void CodeIndexingRegistry::finishUnknownIndex(const FinishUnknownIndex &task) {
  auto narrowed = this->unknownUse(task.index->object, "");
  if (narrowed.isErr()) {
    return this->reportIndex(task.index,
                             CodeIndexingRegistryCheckIndexErrorCode::UNKNOWN_NOT_NARROWED,
                             narrowed.error().message);
  }

  return this->state.walk.tasks.pushChecked(task.index);
}

void CodeIndexingRegistry::reportUndefinedIndex(const ReportUndefinedIndex &task) {
  auto message = ErrorTextIndexRead::undefined();

  return this->reportIndex(
      task.index, CodeIndexingRegistryCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED, message);
}

void CodeIndexingRegistry::finishPositionIndex(const FinishPositionIndex &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  if (type->isError() || index->index->type->isError()) {
    return this->state.walk.tasks.pushChecked(index);
  }
  if (type->isListParam()) {
    this->typeListParamIndex(index, type);
    return this->state.walk.tasks.pushChecked(index);
  }
  const auto *element = this->indexedElement(type);
  bool isOptionalList =
      type->isOptional() && (type->inner()->isArray() || type->inner()->isString());
  if (element == nullptr && isOptionalList) {
    auto message = ErrorTextIndexRead::undefined();
    return this->reportIndex(index, CodeIndexingRegistryCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED,
                             message);
  }
  if (element == nullptr) {
    auto message = ErrorTextIndexRead::notIndexed(type->name());
    return this->reportIndex(index, CodeIndexingRegistryCheckIndexErrorCode::NOT_INDEXABLE,
                             message);
  }
  bool isInRange = this->state.flow.proofs.canProveInBounds(index->object, index->index);
  ElementIndex *access = index->form.as<ElementIndex>();
  if (access != nullptr) {
    access->mayBeOutOfRange = !isInRange;
  }
  index->type = isInRange ? element : Type::optional(element);

  return this->state.walk.tasks.pushChecked(index);
}

void CodeIndexingRegistry::typeListParamIndex(IndexExpr *index, const Type *type) {
  const auto *at = ExprParts::withoutImplicitCasts(index->index);
  const auto *literal = dyn_cast<IntLitExpr>(at);
  if (literal == nullptr) {
    const auto *element = Type::indexedAccess(type, Type::intTy());
    index->type = Type::optional(element);
    return;
  }
  const auto *position = Type::numberLiteral(double(literal->value), Type::intTy());
  index->type = Type::indexedAccess(type, position);
}

void CodeIndexingRegistry::afterEnumIndexResolved(const AfterEnumIndexResolved &task) {
  this->state.walk.tasks.pushTask(FinishEnumIndex{.index = task.index, .enumDecl = task.enumDecl});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &task.index->index, .expected = Type::intTy()});
}

void CodeIndexingRegistry::finishEnumIndex(const FinishEnumIndex &task) {
  IndexExpr *index = task.index;
  const Type *type = index->index->type;
  if (type->isError()) {
    return this->state.walk.tasks.pushChecked(index);
  }
  if (task.enumDecl->isString) {
    auto message = ErrorTextEnumIndex::strings(task.enumDecl->name);
    return this->reportIndex(index, CodeIndexingRegistryCheckIndexErrorCode::ENUM_STRING, message);
  }
  if (!type->isInteger()) {
    auto message = ErrorTextEnumIndex::notInteger(type->name());
    return this->reportIndex(index, CodeIndexingRegistryCheckIndexErrorCode::ENUM_NOT_INTEGER,
                             message);
  }
  this->state.walk.tasks.pushTask(EnumIndexValue{.index = index, .enumDecl = task.enumDecl});

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &index->index, .to = Type::intTy()});
}

void CodeIndexingRegistry::enumIndexValue(const EnumIndexValue &task) {
  IndexExpr *index = task.index;
  auto value = this->state.flow.measurements.linear(index->index);
  if (value.has_value() && value->term.isZero()) {
    auto *name = this->memberNameAt(task.enumDecl, value->offset, index->loc());
    return this->state.walk.tasks.pushChecked(name);
  }
  index->form = EnumNameIndex{.enumDecl = task.enumDecl};
  index->type = Type::optional(Type::string());

  return this->state.walk.tasks.pushChecked(index);
}

void CodeIndexingRegistry::reportIndex(IndexExpr *index,
                                       CodeIndexingRegistryCheckIndexErrorCode code,
                                       const std::string &message) {
  this->state.diagnostics.error(this->indexErrorLoc(index, code), message);

  return this->state.walk.tasks.pushChecked(index);
}

llvm::SMLoc
CodeIndexingRegistry::indexErrorLoc(const IndexExpr *index,
                                    CodeIndexingRegistryCheckIndexErrorCode code) const {
  switch (code) {
    case CodeIndexingRegistryCheckIndexErrorCode::ENUM_NOT_INTEGER:
    case CodeIndexingRegistryCheckIndexErrorCode::GENERIC_INDEX:
    case CodeIndexingRegistryCheckIndexErrorCode::KEY_REJECTED:
      return index->index->loc();
    case CodeIndexingRegistryCheckIndexErrorCode::UNKNOWN_NOT_NARROWED:
    case CodeIndexingRegistryCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED:
      return index->object->loc();
    case CodeIndexingRegistryCheckIndexErrorCode::ALREADY_REPORTED:
    case CodeIndexingRegistryCheckIndexErrorCode::ENUM_STRING:
    case CodeIndexingRegistryCheckIndexErrorCode::FIELD_REJECTED:
    case CodeIndexingRegistryCheckIndexErrorCode::DICT_INDEX:
    case CodeIndexingRegistryCheckIndexErrorCode::NOT_INDEXABLE:
      return index->loc();
  }

  return index->loc();
}

CodeIndexingRegistry::ResultUnknownNarrowed
CodeIndexingRegistry::unknownUse(const Expr *object, llvm::StringRef what) const {
  auto shown = this->hintName(object);
  if (object->type->isAnyObject()) {
    auto message = ErrorTextUnknownUse::object(shown, what);

    return err(CodeUnknownValuesRegistryUnknownUseError{
        .code = CodeUnknownValuesRegistryUnknownUseErrorCode::OBJECT_NOT_NARROWED,
        .message = message,
    });
  }
  auto message = ErrorTextUnknownUse::unknown(shown, what);

  return err(CodeUnknownValuesRegistryUnknownUseError{
      .code = CodeUnknownValuesRegistryUnknownUseErrorCode::VALUE_NOT_NARROWED,
      .message = message,
  });
}

std::string CodeIndexingRegistry::hintName(const Expr *expr) const {
  const Expr *bare = ExprParts::withoutImplicitCasts(expr);
  const auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr) {
    return "x";
  }

  return name->name.str();
}

Expr *CodeIndexingRegistry::chainLink(Expr *object) {
  const Type *type = object->type;
  if (type->nonOptional()->isJson()) {
    return this->shortCircuitLink(object, Type::json());
  }
  if (!type->isOptional()) {
    return object;
  }

  return this->shortCircuitLink(object, type->inner());
}

Expr *CodeIndexingRegistry::shortCircuitLink(Expr *object, const Type *type) {
  auto *link = this->state.ast.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type;
  link->shortCircuit = true;
  this->state.walk.functions.markShortCircuit();

  return link;
}

CodeIndexingRegistry::ResultIndexIsInteger
CodeIndexingRegistry::checkPositionType(const Type *type) const {
  if (!type->isInteger()) {
    auto message = ErrorTextPosition::notInt(type->name(), type->isFloat(), type->isOptional());

    return err(CodeIndexingRegistryPositionTypeError{
        .code = CodeIndexingRegistryPositionTypeErrorCode::NOT_AN_INTEGER,
        .message = message,
    });
  }

  return ok(type);
}

const Type *CodeIndexingRegistry::indexedElement(const Type *type) const {
  if (type->isString()) {
    return type;
  }
  if (type->isArray()) {
    return type->inner();
  }

  return nullptr;
}

bool CodeIndexingRegistry::isGenericKey(const Type *key) const {
  return key->isParam() && !key->isListParam() &&
         this->state.types.relations.isExpectingLiteral(key);
}

bool CodeIndexingRegistry::isGenericKeyName(const NameExpr *keyName) const {
  auto entry = this->state.walk.scopes.findName(keyName->name);
  const VarSymbol *variable = entry.var();
  if (variable == nullptr || variable->type == nullptr) {
    return false;
  }

  return this->isGenericKey(variable->type);
}

Expr *CodeIndexingRegistry::memberNameAt(const EnumDecl *enumDecl, int64_t number,
                                         llvm::SMLoc loc) {
  const EnumMember *found = this->lastMemberNumbered(enumDecl, number);
  if (found == nullptr) {
    auto *undefined = this->state.ast.make<UndefinedExpr>(loc);
    undefined->type = Type::undefinedTy();

    return undefined;
  }
  auto name = ToUtf16::toUtf16(found->name);
  auto *literal = this->state.ast.make<StringLitExpr>(loc, name);
  literal->type = Type::string();

  return literal;
}

const EnumMember *CodeIndexingRegistry::lastMemberNumbered(const EnumDecl *enumDecl,
                                                           int64_t number) const {
  for (const EnumMember &member : llvm::reverse(enumDecl->members)) {
    if (member.number == number) {
      return &member;
    }
  }

  return nullptr;
}

}
