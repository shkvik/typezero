#include "compiler/semantic/code/dictionaries/dictionary_access/dictionary_access.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/semantic/bodies/objects/dict_check_helpers/dict_check_helpers.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

DictionaryAccess::DictionaryAccess(SemanticState &owner)
    : state(owner) {}

void DictionaryAccess::checkRecordKeySlot(const CheckRecordKeySlot &task) {
  this->state.walk.tasks.pushTask(FinishRecordKey{.slot = task.slot});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = task.slot, .expected = Type::string()});
}

void DictionaryAccess::finishRecordKey(const FinishRecordKey &task) {
  const Expr *checked = *task.slot;
  auto text = this->checkStringKey(checked->type);
  if (text.isErr()) {
    return this->state.diagnostics.error(checked->loc(), text.error().message);
  }
  auto what = ErrorTextKeyType::keyLabel();

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = task.slot, .to = Type::string(), .what = what});
}

void DictionaryAccess::checkDictMember(const CheckDictMember &task) {
  MemberExpr *member = task.member;
  auto access = this->checkMemberAccess(member);
  if (access.isErr()) {
    this->state.diagnostics.error(member->loc(), access.error().message);
    return this->state.walk.tasks.pushChecked(member);
  }

  return this->state.walk.tasks.pushChecked(access.value());
}

void DictionaryAccess::checkDictIndex(const CheckDictIndex &task) {
  IndexExpr *index = task.index;
  if (index->object->type->isRecord()) {
    this->state.walk.tasks.pushTask(FinishDictIndexRead{.index = index});
    return this->state.walk.tasks.pushTask(CheckRecordKeySlot{.slot = &index->index});
  }
  this->state.walk.tasks.pushTask(FinishDictIndexError{.index = index});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
}

void DictionaryAccess::finishDictIndexRead(const FinishDictIndexRead &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  auto getName = DictMethodTable::getName(DictMethod::GET);
  const auto *valueType = Type::optional(type->valueType());
  auto *read = this->dictCall(index->loc(), index->object, getName, Builtin::MapGet, {index->index},
                              valueType);

  return this->state.walk.tasks.pushChecked(read);
}

void DictionaryAccess::finishDictIndexError(const FinishDictIndexError &task) {
  IndexExpr *index = task.index;
  index->type = Type::error();
  this->state.walk.tasks.pushChecked(index);
  auto indexed = this->checkCollectionIndex(index);
  if (indexed.isErr()) {
    return this->state.diagnostics.error(index->loc(), indexed.error().message);
  }
}

DictionaryAccess::ResultDictIndexedValue
DictionaryAccess::checkCollectionIndex(const IndexExpr *index) const {
  const Type *type = index->object->type;
  auto text = DictCheckHelpers::dictText(index->object, type);
  if (type->isMap()) {
    auto message = ErrorTextDictIndex::mapIndex(text);

    return err(CodeDictionariesRegistryCheckDictIndexError{
        .code = CodeDictionariesRegistryCheckDictIndexErrorCode::MAP_INDEX,
        .message = message,
    });
  }
  if (type->isSet()) {
    auto message = ErrorTextDictIndex::setIndex(text);

    return err(CodeDictionariesRegistryCheckDictIndexError{
        .code = CodeDictionariesRegistryCheckDictIndexErrorCode::SET_INDEX,
        .message = message,
    });
  }

  return ok(type);
}

void DictionaryAccess::checkDictTarget(const CheckDictTarget &task) {
  Expr *target = task.target;
  auto *index = dyn_cast<IndexExpr>(target);
  target->type = Type::error();
  this->state.walk.tasks.pushTask(FinishDictTarget{.target = target, .form = task.form});
  if (index == nullptr) {
    return;
  }
  if (index->object->type->isRecord()) {
    return this->state.walk.tasks.pushTask(CheckRecordKeySlot{.slot = &index->index});
  }

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
}

void DictionaryAccess::finishDictTarget(const FinishDictTarget &task) {
  Expr *target = task.target;
  auto *index = dyn_cast<IndexExpr>(target);
  auto *member = dyn_cast<MemberExpr>(target);
  const Expr *object = index != nullptr ? index->object : member->object;
  auto value = object->type->isRecord() ? this->checkRecordTarget(index, member, object, task.form)
                                        : this->checkCollectionTarget(index, member, object);
  if (value.isErr()) {
    this->state.diagnostics.error(target->loc(), value.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  target->type = value.value();

  return this->state.walk.tasks.pushAnswer(value.value());
}

DictionaryAccess::ResultRecordTargetType
DictionaryAccess::checkRecordTarget(const IndexExpr *index, const MemberExpr *member,
                                    const Expr *object, AssignForm form) const {
  const Type *type = object->type;
  if (form == AssignForm::COMPOUND) {
    auto text = DictCheckHelpers::dictText(object, type);
    std::string key = index != nullptr ? text + "[k]" : text + "." + member->name.str();
    auto hint = this->missingHint(key, type->valueType());
    auto message = ErrorTextDictTarget::missing(key, hint);

    return err(CodeDictionariesRegistryCheckDictTargetError{
        .code = CodeDictionariesRegistryCheckDictTargetErrorCode::MAY_BE_MISSING,
        .message = message,
    });
  }

  return ok(type->valueType());
}

DictionaryAccess::ResultRecordTargetType
DictionaryAccess::checkCollectionTarget(const IndexExpr *index, const MemberExpr *member,
                                        const Expr *object) const {
  const Type *type = object->type;
  auto text = DictCheckHelpers::dictText(object, type);
  if (type->isMap()) {
    std::string access = index != nullptr ? std::string("[key]") : "." + member->name.str();
    auto message = ErrorTextDictTarget::mapSet(text, access);

    return err(CodeDictionariesRegistryCheckDictTargetError{
        .code = CodeDictionariesRegistryCheckDictTargetErrorCode::MAP_TARGET,
        .message = message,
    });
  }
  bool isSize =
      member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::SIZE);
  if (isSize) {
    auto message = ErrorTextDictTarget::setSize(text);

    return err(CodeDictionariesRegistryCheckDictTargetError{
        .code = CodeDictionariesRegistryCheckDictTargetErrorCode::SET_SIZE,
        .message = message,
    });
  }
  auto message = ErrorTextDictTarget::setIndex(text);

  return err(CodeDictionariesRegistryCheckDictTargetError{
      .code = CodeDictionariesRegistryCheckDictTargetErrorCode::SET_INDEX,
      .message = message,
  });
}

void DictionaryAccess::checkRecordNullishAssign(const CheckRecordNullishAssign &task) {
  this->state.walk.tasks.pushTask(
      AfterNullishRecordObject{.assign = task.assign, .index = task.index});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &task.index->object, .expected = nullptr});
}

void DictionaryAccess::afterNullishRecordObject(const AfterNullishRecordObject &task) {
  if (!task.index->object->type->isRecord()) {
    return this->state.walk.tasks.pushChecked(nullptr);
  }
  this->state.walk.tasks.pushTask(
      AfterNullishRecordKey{.assign = task.assign, .index = task.index});

  return this->state.walk.tasks.pushTask(CheckRecordKeySlot{.slot = &task.index->index});
}

void DictionaryAccess::afterNullishRecordKey(const AfterNullishRecordKey &task) {
  const Type *valueType = task.index->object->type->valueType();
  auto before = this->state.walk.flow.getFlow();
  this->state.walk.tasks.pushTask(FinishNullishRecord{
      .assign = task.assign,
      .index = task.index,
      .before = std::move(before),
  });

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &task.assign->value, .expected = valueType});
}

void DictionaryAccess::finishNullishRecord(const FinishNullishRecord &task) {
  AssignExpr *assign = task.assign;
  IndexExpr *index = task.index;
  const Type *valueType = index->object->type->valueType();
  const Flow &after = this->state.walk.flow.getFlow();
  auto joined = this->state.flow.joins.join(task.before, after);
  this->state.walk.flow.replaceFlow(std::move(joined));
  const Type *given = assign->value->type;
  bool isDefinite = !given->isOptional() && !given->isUndefined();
  const Type *stored = isDefinite ? valueType->nonOptional() : valueType;
  auto setName = DictMethodTable::getName(DictMethod::SET);
  auto *set = this->dictCall(assign->loc(), index->object, setName, Builtin::RecordNullishSet,
                             {index->index, assign->value}, stored);
  auto *call = cast<CallExpr>(set);
  this->state.walk.tasks.pushChecked(set);

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = &call->args[1], .to = valueType, .what = ""});
}

void DictionaryAccess::checkDictDelete(const CheckDictDelete &task) {
  UnaryExpr *unary = task.unary;
  auto *member = dyn_cast<MemberExpr>(unary->operand);
  auto *index = dyn_cast<IndexExpr>(unary->operand);
  Expr *object = member != nullptr ? member->object : index->object;
  unary->type = Type::boolTy();
  auto removable = this->checkRecordDelete(object);
  if (removable.isErr()) {
    this->state.walk.tasks.pushChecked(unary);
    this->state.walk.tasks.pushTask(
        ReportError{.loc = unary->loc(), .message = removable.error().message});
    if (index != nullptr) {
      this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
    }
    return;
  }
  if (member != nullptr) {
    auto *key = this->keyLiteral(unary->loc(), member->name);
    return this->pushDictRemoval(unary, object, key);
  }
  this->state.walk.tasks.pushTask(FinishDictDelete{.unary = unary});

  return this->state.walk.tasks.pushTask(CheckRecordKeySlot{.slot = &index->index});
}

void DictionaryAccess::finishDictDelete(const FinishDictDelete &task) {
  auto *index = cast<IndexExpr>(task.unary->operand);

  return this->pushDictRemoval(task.unary, index->object, index->index);
}

DictionaryAccess::ResultRecordKeyDeleted
DictionaryAccess::checkRecordDelete(const Expr *object) const {
  const Type *type = object->type;
  if (!type->isRecord()) {
    auto text = DictCheckHelpers::dictText(object, type);
    auto kind = this->collectionName(type);
    auto what = ErrorTextDictDelete::entryPart(type->isMap());
    auto message = ErrorTextDictDelete::property(kind, text, what);

    return err(CodeDictionariesRegistryCheckDictDeleteError{
        .code = CodeDictionariesRegistryCheckDictDeleteErrorCode::NOT_A_RECORD,
        .message = message,
    });
  }

  return ok(type);
}

void DictionaryAccess::pushDictRemoval(UnaryExpr *unary, Expr *object, Expr *key) {
  auto *yes = this->state.ast.make<BoolLitExpr>(unary->loc(), true);
  yes->type = Type::boolTy();
  auto deleteName = DictMethodTable::getName(DictMethod::DELETE_KEY);
  auto *removal =
      this->dictCall(unary->loc(), object, deleteName, Builtin::MapDelete, {key}, Type::boolTy());
  auto *sequence =
      this->state.ast.make<SequenceExpr>(unary->loc(), std::vector<Expr *>{removal, yes});
  sequence->type = Type::boolTy();

  return this->state.walk.tasks.pushChecked(sequence);
}

void DictionaryAccess::checkDictIn(const CheckDictIn &task) {
  BinaryExpr *binary = task.binary;
  auto operands = this->checkInOperands(binary);
  if (operands.isErr()) {
    llvm::SMLoc at = this->inErrorLoc(binary, operands.error().code);
    this->state.diagnostics.error(at, operands.error().message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  auto hasName = DictMethodTable::getName(DictMethod::HAS);
  auto *has = this->dictCall(binary->loc(), binary->rhs, hasName, Builtin::MapHas, {binary->lhs},
                             Type::boolTy());
  auto *call = cast<CallExpr>(has);
  this->state.walk.tasks.pushChecked(has);
  auto what = ErrorTextKeyType::keyLabel();

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = &call->args[0], .to = Type::string(), .what = what});
}

DictionaryAccess::ResultRecordKeyChecked
DictionaryAccess::checkInOperands(const BinaryExpr *binary) const {
  const Type *type = binary->rhs->type->nonOptional();
  if (binary->rhs->type->isOptional()) {
    auto message = ErrorTextDictIn::undefined();

    return err(CodeDictionariesRegistryCheckDictInError{
        .code = CodeDictionariesRegistryCheckDictInErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isRecord()) {
    auto text = DictCheckHelpers::dictText(binary->rhs, type);
    auto kind = this->collectionName(type);
    auto what = ErrorTextDictDelete::entryPart(type->isMap());
    auto message = ErrorTextDictIn::property(kind, text, what);

    return err(CodeDictionariesRegistryCheckDictInError{
        .code = CodeDictionariesRegistryCheckDictInErrorCode::NOT_A_RECORD,
        .message = message,
    });
  }
  const Type *key = binary->lhs->type;
  if (key->nonOptional()->isNumeric()) {
    auto message = ErrorTextRecordKey::numberIn(key->name());

    return err(CodeDictionariesRegistryCheckDictInError{
        .code = CodeDictionariesRegistryCheckDictInErrorCode::NUMBER_KEY,
        .message = message,
    });
  }

  return ok(type);
}

llvm::SMLoc DictionaryAccess::inErrorLoc(const BinaryExpr *binary,
                                         CodeDictionariesRegistryCheckDictInErrorCode code) const {
  switch (code) {
    case CodeDictionariesRegistryCheckDictInErrorCode::MAY_BE_UNDEFINED:
      return binary->rhs->loc();
    case CodeDictionariesRegistryCheckDictInErrorCode::NUMBER_KEY:
      return binary->lhs->loc();
    case CodeDictionariesRegistryCheckDictInErrorCode::NOT_A_RECORD:
      return binary->loc();
  }

  return binary->loc();
}

Expr *DictionaryAccess::dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name,
                                 Builtin builtin, std::vector<Expr *> args, const Type *type) {
  auto *member = this->state.ast.make<MemberExpr>(loc, receiver, name);
  member->type = type;
  auto *call = this->state.ast.make<CallExpr>(loc, member, std::move(args));
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;

  return call;
}

Expr *DictionaryAccess::keyLiteral(llvm::SMLoc loc, llvm::StringRef name) {
  auto *key = this->state.ast.make<StringLitExpr>(loc, ToUtf16::toUtf16(name));
  key->type = Type::string();

  return key;
}

DictionaryAccess::ResultTextKeyForRecord DictionaryAccess::checkStringKey(const Type *type) const {
  if (type->nonOptional()->isNumeric()) {
    auto message = ErrorTextRecordKey::number(type->name());

    return err(CodeDictionariesRegistryStringKeyError{
        .code = CodeDictionariesRegistryStringKeyErrorCode::NUMBER_KEY,
        .message = message,
    });
  }

  return ok(type);
}

DictionaryAccess::ResultDictMemberAccess DictionaryAccess::checkMemberAccess(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::SMLoc loc = member->loc();
  if (type->isRecord()) {
    auto *key = this->keyLiteral(loc, member->name);
    auto *read = this->dictCall(loc, member->object, DictMethodTable::getName(DictMethod::GET),
                                Builtin::MapGet, {key}, Type::optional(type->valueType()));

    return ok(read);
  }
  if (BuiltinPropertyTable::isName(member->name, BuiltinProperty::SIZE)) {
    auto *size =
        this->dictCall(loc, member->object, BuiltinPropertyTable::getName(BuiltinProperty::SIZE),
                       Builtin::MapSize, {}, Type::intTy());

    return ok(size);
  }
  auto text = DictCheckHelpers::dictText(member->object, type);
  const auto *found = DictMethodTable::find(member->name);
  bool isMethod = found != nullptr && this->isMethodOn(*found, type);
  member->type = Type::error();
  if (isMethod) {
    auto message = ErrorTextDictMember::callOnly(text, member->name);

    return err(CodeDictionariesRegistryCheckDictMemberError{
        .code = CodeDictionariesRegistryCheckDictMemberErrorCode::METHOD_VALUE,
        .message = message,
    });
  }
  auto message = ErrorTextDictMember::noProperty(member->name, type->name(), text);

  return err(CodeDictionariesRegistryCheckDictMemberError{
      .code = CodeDictionariesRegistryCheckDictMemberErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

bool DictionaryAccess::isMethodOn(const DictMethodInfo &method, const Type *type) const {
  if (type->isMap()) {
    return DictMethodTable::isOnMap(method);
  }

  return DictMethodTable::isOnSet(method);
}

std::string DictionaryAccess::missingHint(const std::string &key, const Type *value) const {
  if (value->isNumeric() || value->isDecimal()) {
    return ErrorTextDictTarget::numberHint(key);
  }
  if (value->isString()) {
    return ErrorTextDictTarget::textHint(key);
  }

  return "";
}

llvm::StringRef DictionaryAccess::collectionName(const Type *type) const {
  if (type->isMap()) {
    return GlobalNameTable::getName(GlobalName::MAP);
  }

  return GlobalNameTable::getName(GlobalName::SET);
}

}
