#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
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
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->argListChecker = &all.argListChecker;
  this->weakRefChecker = &all.weakRefChecker;
  this->iterableChecker = &all.iterableChecker;
}

std::u16string DictChecker::toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

DictChecker::ResultDictTypeResolved DictChecker::resolveDictType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (TypeNameTable::isName(name, TypeName::WEAK_REF)) {
    const Type *weak = this->weakRefChecker->resolveWeakRefType(ref);

    return ok(weak);
  }
  const TypeNameInfo *builtin = TypeNameTable::find(name);
  bool isWeakCollection = builtin != nullptr && builtin->form.is<WeakCollectionTypeName>();
  if (isWeakCollection) {
    std::string message = ErrorTextDictType::weak(name, name.drop_front(4));

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
    std::string message = isSet   ? ErrorTextDictType::setArgs(ref.name)
                          : isMap ? ErrorTextDictType::mapArgs(ref.name)
                                  : ErrorTextDictType::recordArgs();

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const Type *key = this->typeResolver->resolveType(ref.args[0], false);
  const Type *value = isSet ? Type::voidTy() : this->typeResolver->resolveType(ref.args[1], false);
  if (key->isError() || value->isError()) {
    return ok(Type::error());
  }
  if (collection == Collection::RECORD) {
    return DictChecker::recordType(key, value);
  }
  ResultKeyTypeIsAllowed allowed = this->checkKeyType(key);
  if (allowed.isErr()) {
    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::BAD_KEY_TYPE,
        .message = allowed.error().message,
    });
  }
  const Type *dict = Type::dict(isMap ? Type::DictKind::Map : Type::DictKind::Set, key, value);

  return ok(dict);
}

DictChecker::ResultDictTypeResolved DictChecker::recordType(const Type *key, const Type *value) {
  if (DictChecker::isLiteralKeys(key)) {
    const Type *object = DictChecker::literalKeysObject(key, value);

    return ok(object);
  }
  if (!key->isString() && key->nonOptional()->isNumeric()) {
    std::string message = ErrorTextDictType::numberKeys(key->name(), value->name());

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::NUMBER_KEYS,
        .message = message,
    });
  }
  if (!key->isString()) {
    std::string message = ErrorTextDictType::otherKeys(key->name(), value->name());

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::OTHER_KEYS,
        .message = message,
    });
  }
  const Type *dict = Type::dict(Type::DictKind::Record, key, value);

  return ok(dict);
}

bool DictChecker::isLiteralKeys(const Type *key) {
  if (key->isStringLiteral()) {
    return true;
  }
  if (!key->isUnion()) {
    return false;
  }
  for (const Type *m : key->members()) {
    if (!m->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

const Type *DictChecker::literalKeysObject(const Type *key, const Type *value) {
  llvm::ArrayRef<const Type *> literals =
      key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
  std::vector<Field> fields;
  for (const Type *m : literals) {
    std::string text = ToUtf8::toUtf8(m->literalText());
    fields.push_back({text, value, false});
  }

  return Type::object(std::move(fields));
}

bool DictChecker::keyArgument(DictCheckerResolveDictTypeErrorCode code) {
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
    std::string message = ErrorTextKeyType::unknown();

    return err(DictCheckerCheckKeyTypeError{
        .code = DictCheckerCheckKeyTypeErrorCode::UNKNOWN_KEY,
        .message = message,
    });
  }
  if (type->nonOptional()->isResult()) {
    std::string message = ErrorTextKeyType::result();

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
  if (Expr *iterator = this->iterableChecker->userIterator(expr)) {
    return iterator;
  }
  if (type && type->isParam() && type->iterated()) {
    return this->typeRelations->convert(expr, Type::iterator(type->iterated()));
  }
  if (type && type->isListParam()) {
    return this->typeRelations->convert(expr,
                                        Type::iterator(this->argListChecker->listElement(type)));
  }
  if (type && type->intlKind() == IntlKind::Segments) {
    return this->intlChecker->segmentsArray(expr);
  }
  if (!type || (!type->isMap() && !type->isSet())) {
    return expr;
  }
  if (type->isMap()) {
    return this->dictCall(expr->loc(), expr, DictMethodTable::getName(DictMethod::ENTRIES),
                          Builtin::MapEntries, {},
                          Type::iterator(Type::tuple({type->keyType(), type->valueType()})));
  }

  return this->dictCall(expr->loc(), expr, DictMethodTable::getName(DictMethod::VALUES),
                        Builtin::SetValues, {}, Type::iterator(type->keyType()));
}

Expr *DictChecker::keyLiteral(llvm::SMLoc loc, llvm::StringRef name) {
  auto *key = this->state.getAst().make<StringLitExpr>(loc, DictChecker::toUtf16(name));
  key->type = Type::string();

  return key;
}

DictChecker::ResultTextKeyForRecord DictChecker::stringKey(const Type *type) {
  if (type->nonOptional()->isNumeric()) {
    std::string message = ErrorTextRecordKey::number(type->name());

    return err(DictCheckerStringKeyError{
        .code = DictCheckerStringKeyErrorCode::NUMBER_KEY,
        .message = message,
    });
  }

  return ok(type);
}

Expr *DictChecker::checkRecordKey(Expr *key) {
  key = this->exprChecker->checkExpr(key, Type::string());
  ResultTextKeyForRecord text = DictChecker::stringKey(key->type);
  if (text.isErr()) {
    this->state.getDiagnostics().error(key->loc(), text.error().message);
    return key;
  }

  std::string what = ErrorTextKeyType::keyLabel();

  return this->typeRelations->coerce(key, Type::string(), what);
}

DictChecker::ResultDictMemberAccess DictChecker::checkDictMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::SMLoc loc = member->loc();
  if (type->isRecord()) {
    Expr *key = this->keyLiteral(loc, member->name);
    Expr *read = this->dictCall(loc, member->object, DictMethodTable::getName(DictMethod::GET),
                                Builtin::MapGet, {key}, Type::optional(type->valueType()));

    return ok(read);
  }
  if (BuiltinPropertyTable::isName(member->name, BuiltinProperty::SIZE)) {
    Expr *size =
        this->dictCall(loc, member->object, BuiltinPropertyTable::getName(BuiltinProperty::SIZE),
                       Builtin::MapSize, {}, Type::intTy());

    return ok(size);
  }
  std::string text = DictCheckHelpers::dictText(member->object, type);
  const DictMethodInfo *found = DictMethodTable::find(member->name);
  bool isMethod = found != nullptr && (type->isMap() ? DictMethodTable::isOnMap(*found)
                                                     : DictMethodTable::isOnSet(*found));
  member->type = Type::error();
  if (isMethod) {
    std::string message = ErrorTextDictMember::callOnly(text, member->name);

    return err(DictCheckerCheckDictMemberError{
        .code = DictCheckerCheckDictMemberErrorCode::METHOD_VALUE,
        .message = message,
    });
  }
  std::string message = ErrorTextDictMember::noProperty(member->name, type->name(), text);

  return err(DictCheckerCheckDictMemberError{
      .code = DictCheckerCheckDictMemberErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

DictChecker::ResultDictIndexedValue DictChecker::checkDictIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  if (type->isRecord()) {
    Expr *key = this->checkRecordKey(index->index);
    Expr *read =
        this->dictCall(index->loc(), index->object, DictMethodTable::getName(DictMethod::GET),
                       Builtin::MapGet, {key}, Type::optional(type->valueType()));

    return ok(read);
  }
  index->index = this->exprChecker->checkExpr(index->index);
  std::string text = DictCheckHelpers::dictText(index->object, type);
  index->type = Type::error();
  if (type->isMap()) {
    std::string message = ErrorTextDictIndex::mapIndex(text);

    return err(DictCheckerCheckDictIndexError{
        .code = DictCheckerCheckDictIndexErrorCode::MAP_INDEX,
        .message = message,
    });
  }
  std::string message = ErrorTextDictIndex::setIndex(text);

  return err(DictCheckerCheckDictIndexError{
      .code = DictCheckerCheckDictIndexErrorCode::SET_INDEX,
      .message = message,
  });
}

DictChecker::ResultRecordTargetType DictChecker::checkDictTarget(Expr *target, bool compound) {
  auto *index = dyn_cast<IndexExpr>(target);
  auto *member = dyn_cast<MemberExpr>(target);
  Expr *object = index ? index->object : member->object;
  const Type *type = object->type;
  target->type = Type::error();
  std::string text = DictCheckHelpers::dictText(object, type);
  if (!type->isRecord() && index) {
    index->index = this->exprChecker->checkExpr(index->index);
  }
  if (type->isMap()) {
    std::string access = index ? std::string("[key]") : "." + member->name.str();
    std::string message = ErrorTextDictTarget::mapSet(text, access);

    return err(DictCheckerCheckDictTargetError{
        .code = DictCheckerCheckDictTargetErrorCode::MAP_TARGET,
        .message = message,
    });
  }
  if (!type->isRecord() && member &&
      BuiltinPropertyTable::isName(member->name, BuiltinProperty::SIZE)) {
    std::string message = ErrorTextDictTarget::setSize(text);

    return err(DictCheckerCheckDictTargetError{
        .code = DictCheckerCheckDictTargetErrorCode::SET_SIZE,
        .message = message,
    });
  }
  if (!type->isRecord()) {
    std::string message = ErrorTextDictTarget::setIndex(text);

    return err(DictCheckerCheckDictTargetError{
        .code = DictCheckerCheckDictTargetErrorCode::SET_INDEX,
        .message = message,
    });
  }
  if (index) {
    index->index = this->checkRecordKey(index->index);
  }
  if (compound) {
    std::string key = index ? text + "[k]" : text + "." + member->name.str();
    const Type *value = type->valueType();
    std::string hint = value->isNumeric() || value->isDecimal()
                           ? ErrorTextDictTarget::numberHint(key)
                       : value->isString() ? ErrorTextDictTarget::textHint(key)
                                           : "";
    std::string message = ErrorTextDictTarget::missing(key, hint);

    return err(DictCheckerCheckDictTargetError{
        .code = DictCheckerCheckDictTargetErrorCode::MAY_BE_MISSING,
        .message = message,
    });
  }
  target->type = type->valueType();

  return ok(type->valueType());
}

Expr *DictChecker::recordAssign(AssignExpr *assign) {
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  Expr *object = index ? index->object : member ? member->object : nullptr;
  if (!object || !object->type || !object->type->isRecord() || assign->op) {
    return nullptr;
  }
  Expr *key = index ? index->index : nullptr;
  if (!key) {
    key = this->keyLiteral(member->loc(), member->name);
  }

  return this->dictCall(assign->loc(), object, DictMethodTable::getName(DictMethod::SET),
                        Builtin::RecordSet, {key, assign->value}, object->type->valueType());
}

Expr *DictChecker::checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index) {
  index->object = this->exprChecker->checkExpr(index->object);
  const Type *type = index->object->type;
  if (!type->isRecord()) {
    return nullptr;
  }
  Expr *key = this->checkRecordKey(index->index);
  const Type *valueType = type->valueType();
  Flow before = this->state.getFlowState().getFlow();
  Expr *value = this->exprChecker->checkExpr(assign->value, valueType);
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(before, this->state.getFlowState().getFlow()));
  bool isDefinite = !value->type->isOptional() && !value->type->isUndefined();
  value = this->typeRelations->coerce(value, valueType);

  return this->dictCall(assign->loc(), index->object, DictMethodTable::getName(DictMethod::SET),
                        Builtin::RecordNullishSet, {key, value},
                        isDefinite ? valueType->nonOptional() : valueType);
}

DictChecker::ResultRecordKeyDeleted
DictChecker::checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr, llvm::StringRef name) {
  const Type *type = object->type;
  unary->type = Type::boolTy();
  if (!type->isRecord()) {
    std::string text = DictCheckHelpers::dictText(object, type);
    if (keyExpr) {
      keyExpr = this->exprChecker->checkExpr(keyExpr);
    }
    const char *kind = type->isMap() ? "Map" : "Set";
    const char *what = type->isMap() ? "key" : "value";
    std::string message = ErrorTextDictDelete::property(kind, text, what);

    return err(DictCheckerCheckDictDeleteError{
        .code = DictCheckerCheckDictDeleteErrorCode::NOT_A_RECORD,
        .message = message,
    });
  }
  Expr *key = keyExpr ? this->checkRecordKey(keyExpr) : this->keyLiteral(unary->loc(), name);
  auto *yes = this->state.getAst().make<BoolLitExpr>(unary->loc(), true);
  yes->type = Type::boolTy();
  auto *seq = this->state.getAst().make<SequenceExpr>(
      unary->loc(),
      std::vector<Expr *>{this->dictCall(unary->loc(), object,
                                         DictMethodTable::getName(DictMethod::DELETE_KEY),
                                         Builtin::MapDelete, {key}, Type::boolTy()),
                          yes});
  seq->type = Type::boolTy();

  return ok(seq);
}

DictChecker::ResultRecordKeyChecked DictChecker::checkDictIn(BinaryExpr *bin) {
  const Type *type = bin->rhs->type->nonOptional();
  if (bin->rhs->type->isOptional()) {
    std::string message = ErrorTextDictIn::undefined();

    return err(DictCheckerCheckDictInError{
        .code = DictCheckerCheckDictInErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isRecord()) {
    std::string text = DictCheckHelpers::dictText(bin->rhs, type);
    const char *kind = type->isMap() ? "Map" : "Set";
    const char *what = type->isMap() ? "key" : "value";
    std::string message = ErrorTextDictIn::property(kind, text, what);

    return err(DictCheckerCheckDictInError{
        .code = DictCheckerCheckDictInErrorCode::NOT_A_RECORD,
        .message = message,
    });
  }
  Expr *key = bin->lhs;
  if (key->type->nonOptional()->isNumeric()) {
    std::string message = ErrorTextRecordKey::numberIn(key->type->name());

    return err(DictCheckerCheckDictInError{
        .code = DictCheckerCheckDictInErrorCode::NUMBER_KEY,
        .message = message,
    });
  }
  std::string what = ErrorTextKeyType::keyLabel();
  key = this->typeRelations->coerce(key, Type::string(), what);
  Expr *has = this->dictCall(bin->loc(), bin->rhs, DictMethodTable::getName(DictMethod::HAS),
                             Builtin::MapHas, {key}, Type::boolTy());

  return ok(has);
}

}
