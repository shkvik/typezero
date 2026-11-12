#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/bodies/objects/dict_check_helpers/dict_check_helpers.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

DictChecker::DictChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->intlChecker = &all.intlChecker;
  this->flowChecker = &all.flowChecker;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->argListChecker = &all.argListChecker;
  this->weakRefChecker = &all.weakRefChecker;
  this->iterableChecker = &all.iterableChecker;
}

DictChecker::ResultDictTypeResolved DictChecker::resolveDictType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (TypeNameTable::isName(name, TypeName::WEAK_REF)) {
    const auto *weak = this->weakRefChecker->resolveWeakRefType(ref);

    return ok(weak);
  }
  const auto *builtin = TypeNameTable::find(name);
  bool isWeakCollection = builtin != nullptr && builtin->form.is<WeakCollectionTypeName>();
  if (isWeakCollection) {
    auto message = ErrorTextDictType::weak(name, name.drop_front(4));

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::WEAK_COLLECTION,
        .message = message,
    });
  }
  const CollectionTypeName *collectionName =
      builtin != nullptr ? builtin->form.as<CollectionTypeName>() : nullptr;
  if (collectionName == nullptr) {
    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::NOT_A_DICT,
        .message = "",
    });
  }

  return this->resolveCollectionType(ref, collectionName->collection);
}

DictChecker::ResultDictTypeResolved DictChecker::resolveCollectionType(const TypeRef &ref,
                                                                       Collection collection) {
  bool isMap = collection == Collection::MAP;
  bool isSet = collection == Collection::SET;
  size_t wanted = isSet ? 1 : 2;
  if (ref.kind() != TypeRefKind::GENERIC_REF || ref.args.size() != wanted) {
    auto message = this->typeArgumentsText(ref, collection);

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const auto *key = this->typeResolver->resolveType(ref.args[0]);
  const Type *value = isSet ? Type::voidTy() : this->typeResolver->resolveType(ref.args[1]);
  if (key->isError() || value->isError()) {
    return ok(Type::error());
  }
  if (collection == Collection::RECORD) {
    return this->checkRecordType(key, value);
  }
  auto allowed = this->checkKeyType(key);
  if (allowed.isErr()) {
    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::BAD_KEY_TYPE,
        .message = allowed.error().message,
    });
  }
  auto kind = isMap ? Type::DictKind::Map : Type::DictKind::Set;
  const auto *dict = Type::dict(kind, key, value);

  return ok(dict);
}

std::string DictChecker::typeArgumentsText(const TypeRef &ref, Collection collection) const {
  switch (collection) {
    case Collection::SET:
      return ErrorTextDictType::setArgs(ref.name);
    case Collection::MAP:
      return ErrorTextDictType::mapArgs(ref.name);
    case Collection::RECORD:
      return ErrorTextDictType::recordArgs();
  }

  return ErrorTextDictType::recordArgs();
}

DictChecker::ResultDictTypeResolved DictChecker::checkRecordType(const Type *key,
                                                                 const Type *value) const {
  if (this->isLiteralKeys(key)) {
    const auto *object = this->literalKeysObject(key, value);

    return ok(object);
  }
  if (!key->isString() && key->nonOptional()->isNumeric()) {
    auto message = ErrorTextDictType::numberKeys(key->name(), value->name());

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::NUMBER_KEYS,
        .message = message,
    });
  }
  if (!key->isString()) {
    auto message = ErrorTextDictType::otherKeys(key->name(), value->name());

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::OTHER_KEYS,
        .message = message,
    });
  }
  const auto *dict = Type::dict(Type::DictKind::Record, key, value);

  return ok(dict);
}

bool DictChecker::isLiteralKeys(const Type *key) const {
  if (key->isStringLiteral()) {
    return true;
  }
  if (!key->isUnion()) {
    return false;
  }
  for (const Type *member : key->members()) {
    if (!member->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

const Type *DictChecker::literalKeysObject(const Type *key, const Type *value) const {
  llvm::ArrayRef<const Type *> literals =
      key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
  auto fields = std::vector<Field>();
  for (const Type *member : literals) {
    auto text = ToUtf8::toUtf8(member->literalText());
    fields.push_back({text, value, false});
  }

  return Type::object(std::move(fields));
}

bool DictChecker::isKeyArgument(DictCheckerResolveDictTypeErrorCode code) const {
  switch (code) {
    case DictCheckerResolveDictTypeErrorCode::NUMBER_KEYS:
    case DictCheckerResolveDictTypeErrorCode::OTHER_KEYS:
    case DictCheckerResolveDictTypeErrorCode::BAD_KEY_TYPE:
      return true;
    case DictCheckerResolveDictTypeErrorCode::NOT_A_DICT:
    case DictCheckerResolveDictTypeErrorCode::WEAK_COLLECTION:
    case DictCheckerResolveDictTypeErrorCode::WRONG_TYPE_ARGUMENTS:
      return false;
  }

  return false;
}

DictChecker::ResultKeyTypeIsAllowed DictChecker::checkKeyType(const Type *type) {
  if (type->isUnknown()) {
    auto message = ErrorTextKeyType::unknown();

    return err(DictCheckerCheckKeyTypeError{
        .code = DictCheckerCheckKeyTypeErrorCode::UNKNOWN_KEY,
        .message = message,
    });
  }
  if (type->nonOptional()->isResult()) {
    auto message = ErrorTextKeyType::result();

    return err(DictCheckerCheckKeyTypeError{
        .code = DictCheckerCheckKeyTypeErrorCode::RESULT_KEY,
        .message = message,
    });
  }

  return ok(type);
}

Expr *DictChecker::dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                            std::vector<Expr *> args, const Type *type) {
  auto *member = this->state.getAst().make<MemberExpr>(loc, receiver, name);
  member->type = type;
  auto *call = this->state.getAst().make<CallExpr>(loc, member, std::move(args));
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;

  return call;
}

Expr *DictChecker::iterableOf(Expr *expr) {
  const Type *type = expr->type;
  auto *iterator = this->iterableChecker->userIterator(expr);
  if (iterator != nullptr) {
    return iterator;
  }
  if (type != nullptr && type->isParam() && type->iterated() != nullptr) {
    const auto *iterated = Type::iterator(type->iterated());
    return this->typeRelations->convert(expr, iterated);
  }
  if (type != nullptr && type->isListParam()) {
    const auto *element = this->argListChecker->listElement(type);
    const auto *listed = Type::iterator(element);
    return this->typeRelations->convert(expr, listed);
  }
  if (type != nullptr && type->intlKind() == IntlKind::Segments) {
    return this->intlChecker->segmentsArray(expr);
  }
  if (type == nullptr || (!type->isMap() && !type->isSet())) {
    return expr;
  }
  if (type->isMap()) {
    const auto *entry = Type::tuple({type->keyType(), type->valueType()});
    auto entries = DictMethodTable::getName(DictMethod::ENTRIES);
    return this->dictCall(expr->loc(), expr, entries, Builtin::MapEntries, {},
                          Type::iterator(entry));
  }
  auto values = DictMethodTable::getName(DictMethod::VALUES);

  return this->dictCall(expr->loc(), expr, values, Builtin::SetValues, {},
                        Type::iterator(type->keyType()));
}

Expr *DictChecker::keyLiteral(llvm::SMLoc loc, llvm::StringRef name) {
  auto *key = this->state.getAst().make<StringLitExpr>(loc, ToUtf16::toUtf16(name));
  key->type = Type::string();

  return key;
}

DictChecker::ResultTextKeyForRecord DictChecker::checkStringKey(const Type *type) const {
  if (type->nonOptional()->isNumeric()) {
    auto message = ErrorTextRecordKey::number(type->name());

    return err(DictCheckerStringKeyError{
        .code = DictCheckerStringKeyErrorCode::NUMBER_KEY,
        .message = message,
    });
  }

  return ok(type);
}

Expr *DictChecker::checkRecordKey(Expr *key) {
  auto *checked = this->exprChecker->checkExpr(key, Type::string());
  auto text = this->checkStringKey(checked->type);
  if (text.isErr()) {
    this->state.getDiagnostics().error(checked->loc(), text.error().message);
    return checked;
  }
  auto what = ErrorTextKeyType::keyLabel();

  return this->typeRelations->coerce(checked, Type::string(), what);
}

DictChecker::ResultDictMemberAccess DictChecker::checkDictMember(MemberExpr *member) {
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

    return err(DictCheckerCheckDictMemberError{
        .code = DictCheckerCheckDictMemberErrorCode::METHOD_VALUE,
        .message = message,
    });
  }
  auto message = ErrorTextDictMember::noProperty(member->name, type->name(), text);

  return err(DictCheckerCheckDictMemberError{
      .code = DictCheckerCheckDictMemberErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

bool DictChecker::isMethodOn(const DictMethodInfo &method, const Type *type) const {
  if (type->isMap()) {
    return DictMethodTable::isOnMap(method);
  }

  return DictMethodTable::isOnSet(method);
}

DictChecker::ResultDictIndexedValue DictChecker::checkDictIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  if (type->isRecord()) {
    auto *key = this->checkRecordKey(index->index);
    auto *read =
        this->dictCall(index->loc(), index->object, DictMethodTable::getName(DictMethod::GET),
                       Builtin::MapGet, {key}, Type::optional(type->valueType()));

    return ok(read);
  }
  index->index = this->exprChecker->checkExpr(index->index);
  auto text = DictCheckHelpers::dictText(index->object, type);
  index->type = Type::error();
  if (type->isMap()) {
    auto message = ErrorTextDictIndex::mapIndex(text);

    return err(DictCheckerCheckDictIndexError{
        .code = DictCheckerCheckDictIndexErrorCode::MAP_INDEX,
        .message = message,
    });
  }
  auto message = ErrorTextDictIndex::setIndex(text);

  return err(DictCheckerCheckDictIndexError{
      .code = DictCheckerCheckDictIndexErrorCode::SET_INDEX,
      .message = message,
  });
}

DictChecker::ResultRecordTargetType DictChecker::checkDictTarget(Expr *target, AssignForm form) {
  auto *index = dyn_cast<IndexExpr>(target);
  auto *member = dyn_cast<MemberExpr>(target);
  Expr *object = index != nullptr ? index->object : member->object;
  const Type *type = object->type;
  target->type = Type::error();
  if (!type->isRecord()) {
    return this->checkCollectionTarget(index, member, object);
  }
  if (index != nullptr) {
    index->index = this->checkRecordKey(index->index);
  }
  if (form == AssignForm::COMPOUND) {
    auto text = DictCheckHelpers::dictText(object, type);
    std::string key = index != nullptr ? text + "[k]" : text + "." + member->name.str();
    auto hint = this->missingHint(key, type->valueType());
    auto message = ErrorTextDictTarget::missing(key, hint);

    return err(DictCheckerCheckDictTargetError{
        .code = DictCheckerCheckDictTargetErrorCode::MAY_BE_MISSING,
        .message = message,
    });
  }
  target->type = type->valueType();

  return ok(type->valueType());
}

DictChecker::ResultRecordTargetType
DictChecker::checkCollectionTarget(IndexExpr *index, const MemberExpr *member, const Expr *object) {
  const Type *type = object->type;
  auto text = DictCheckHelpers::dictText(object, type);
  if (index != nullptr) {
    index->index = this->exprChecker->checkExpr(index->index);
  }
  if (type->isMap()) {
    std::string access = index != nullptr ? std::string("[key]") : "." + member->name.str();
    auto message = ErrorTextDictTarget::mapSet(text, access);

    return err(DictCheckerCheckDictTargetError{
        .code = DictCheckerCheckDictTargetErrorCode::MAP_TARGET,
        .message = message,
    });
  }
  bool isSize =
      member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::SIZE);
  if (isSize) {
    auto message = ErrorTextDictTarget::setSize(text);

    return err(DictCheckerCheckDictTargetError{
        .code = DictCheckerCheckDictTargetErrorCode::SET_SIZE,
        .message = message,
    });
  }
  auto message = ErrorTextDictTarget::setIndex(text);

  return err(DictCheckerCheckDictTargetError{
      .code = DictCheckerCheckDictTargetErrorCode::SET_INDEX,
      .message = message,
  });
}

std::string DictChecker::missingHint(const std::string &key, const Type *value) const {
  if (value->isNumeric() || value->isDecimal()) {
    return ErrorTextDictTarget::numberHint(key);
  }
  if (value->isString()) {
    return ErrorTextDictTarget::textHint(key);
  }

  return "";
}

Expr *DictChecker::recordAssign(AssignExpr *assign) {
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  Expr *object = this->targetObject(index, member);
  bool isRecordSet = object != nullptr && object->type != nullptr && object->type->isRecord() &&
                     !assign->op.has_value();
  if (!isRecordSet) {
    return nullptr;
  }
  Expr *key = index != nullptr ? index->index : this->keyLiteral(member->loc(), member->name);

  return this->dictCall(assign->loc(), object, DictMethodTable::getName(DictMethod::SET),
                        Builtin::RecordSet, {key, assign->value}, object->type->valueType());
}

Expr *DictChecker::targetObject(const IndexExpr *index, const MemberExpr *member) const {
  if (index != nullptr) {
    return index->object;
  }
  if (member != nullptr) {
    return member->object;
  }

  return nullptr;
}

Expr *DictChecker::checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index) {
  index->object = this->exprChecker->checkExpr(index->object);
  const Type *type = index->object->type;
  if (!type->isRecord()) {
    return nullptr;
  }
  auto *key = this->checkRecordKey(index->index);
  const Type *valueType = type->valueType();
  auto before = this->state.getFlowState().getFlow();
  auto *value = this->exprChecker->checkExpr(assign->value, valueType);
  const Flow &after = this->state.getFlowState().getFlow();
  auto joined = this->flowChecker->join(before, after);
  this->state.getFlowState().replaceFlow(std::move(joined));
  bool isDefinite = !value->type->isOptional() && !value->type->isUndefined();
  auto *coerced = this->typeRelations->coerce(value, valueType);
  const Type *stored = isDefinite ? valueType->nonOptional() : valueType;

  return this->dictCall(assign->loc(), index->object, DictMethodTable::getName(DictMethod::SET),
                        Builtin::RecordNullishSet, {key, coerced}, stored);
}

DictChecker::ResultRecordKeyDeleted
DictChecker::checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr, llvm::StringRef name) {
  const Type *type = object->type;
  unary->type = Type::boolTy();
  if (!type->isRecord()) {
    auto text = DictCheckHelpers::dictText(object, type);
    if (keyExpr != nullptr) {
      this->exprChecker->checkExpr(keyExpr);
    }
    auto kind = this->collectionName(type);
    auto what = ErrorTextDictDelete::entryPart(type->isMap());
    auto message = ErrorTextDictDelete::property(kind, text, what);

    return err(DictCheckerCheckDictDeleteError{
        .code = DictCheckerCheckDictDeleteErrorCode::NOT_A_RECORD,
        .message = message,
    });
  }
  Expr *key =
      keyExpr != nullptr ? this->checkRecordKey(keyExpr) : this->keyLiteral(unary->loc(), name);
  auto *yes = this->state.getAst().make<BoolLitExpr>(unary->loc(), true);
  yes->type = Type::boolTy();
  auto deleteName = DictMethodTable::getName(DictMethod::DELETE_KEY);
  auto *removal =
      this->dictCall(unary->loc(), object, deleteName, Builtin::MapDelete, {key}, Type::boolTy());
  auto *sequence =
      this->state.getAst().make<SequenceExpr>(unary->loc(), std::vector<Expr *>{removal, yes});
  sequence->type = Type::boolTy();

  return ok(sequence);
}

llvm::StringRef DictChecker::collectionName(const Type *type) const {
  if (type->isMap()) {
    return GlobalNameTable::getName(GlobalName::MAP);
  }

  return GlobalNameTable::getName(GlobalName::SET);
}

DictChecker::ResultRecordKeyChecked DictChecker::checkDictIn(BinaryExpr *binary) {
  const Type *type = binary->rhs->type->nonOptional();
  if (binary->rhs->type->isOptional()) {
    auto message = ErrorTextDictIn::undefined();

    return err(DictCheckerCheckDictInError{
        .code = DictCheckerCheckDictInErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isRecord()) {
    auto text = DictCheckHelpers::dictText(binary->rhs, type);
    auto kind = this->collectionName(type);
    auto what = ErrorTextDictDelete::entryPart(type->isMap());
    auto message = ErrorTextDictIn::property(kind, text, what);

    return err(DictCheckerCheckDictInError{
        .code = DictCheckerCheckDictInErrorCode::NOT_A_RECORD,
        .message = message,
    });
  }
  Expr *key = binary->lhs;
  if (key->type->nonOptional()->isNumeric()) {
    auto message = ErrorTextRecordKey::numberIn(key->type->name());

    return err(DictCheckerCheckDictInError{
        .code = DictCheckerCheckDictInErrorCode::NUMBER_KEY,
        .message = message,
    });
  }
  auto what = ErrorTextKeyType::keyLabel();
  auto *coerced = this->typeRelations->coerce(key, Type::string(), what);
  auto *has = this->dictCall(binary->loc(), binary->rhs, DictMethodTable::getName(DictMethod::HAS),
                             Builtin::MapHas, {coerced}, Type::boolTy());

  return ok(has);
}

}
