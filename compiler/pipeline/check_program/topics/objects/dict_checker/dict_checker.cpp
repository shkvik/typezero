#include "compiler/pipeline/check_program/topics/objects/dict_checker/dict_checker.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/intl/intl_checker/intl_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/iterable_checker/iterable_checker.h"
#include "compiler/pipeline/check_program/topics/objects/dict_check_helpers/dict_check_helpers.h"
#include "compiler/pipeline/check_program/topics/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

namespace {

const llvm::StringLiteral kMapMethods[] = {"get",  "set",    "has",     "delete", "clear",
                                           "keys", "values", "entries", "forEach"};

const llvm::StringLiteral kSetMethods[] = {"add",          "has",          "delete",
                                           "clear",        "keys",         "values",
                                           "entries",      "forEach",      "union",
                                           "intersection", "difference",   "symmetricDifference",
                                           "isSubsetOf",   "isSupersetOf", "isDisjointFrom"};

}

std::u16string DictChecker::toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

DictChecker::DictChecker(CheckState &owner)
    : state(owner) {}

DictChecker::ResultDictTypeResolved DictChecker::resolveDictType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (name == "WeakRef") {
    const Type *weak = WeakRefChecker(this->state).resolveWeakRefType(ref);

    return ok(weak);
  }
  if (name == "WeakMap" || name == "WeakSet") {
    std::string message = ErrorTextDictType::weak(name, name.drop_front(4));

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::WEAK_COLLECTION,
        .message = message,
    });
  }
  bool record = name == "Record";
  bool map = name == "Map" || name == "ReadonlyMap";
  bool set = name == "Set" || name == "ReadonlySet";
  if (!record && !map && !set) {
    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::NOT_A_DICT,
        .message = "",
    });
  }
  size_t wanted = set ? 1 : 2;
  if (ref.kind() != TypeRefKind::GENERIC_REF || ref.args.size() != wanted) {
    std::string message = set   ? ErrorTextDictType::setArgs(name)
                          : map ? ErrorTextDictType::mapArgs(name)
                                : ErrorTextDictType::recordArgs();

    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const Type *key = TypeResolver(this->state).resolveType(ref.args[0], false);
  const Type *value =
      set ? Type::voidTy() : TypeResolver(this->state).resolveType(ref.args[1], false);
  if (key->isError() || value->isError()) {
    return ok(Type::error());
  }
  if (record) {
    llvm::ArrayRef<const Type *> literals =
        key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
    if (key->isStringLiteral() || (key->isUnion() && llvm::all_of(literals, [](const Type *m) {
                                     return m->isStringLiteral();
                                   }))) {
      std::vector<Field> fields;
      for (const Type *m : literals) {
        const std::u16string &literal = m->literalText();
        std::string text;
        llvm::convertUTF16ToUTF8String(
            llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(literal.data()),
                                        literal.size()),
            text);
        fields.push_back({text, value, false});
      }
      const Type *object = Type::object(std::move(fields));

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
  ResultKeyTypeIsAllowed allowed = this->checkKeyType(key);
  if (allowed.isErr()) {
    return err(DictCheckerResolveDictTypeError{
        .code = DictCheckerResolveDictTypeErrorCode::BAD_KEY_TYPE,
        .message = allowed.error().message,
    });
  }
  const Type *dict = Type::dict(map ? Type::DictKind::Map : Type::DictKind::Set, key, value);

  return ok(dict);
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
  if (Expr *iterator = IterableChecker(this->state).userIterator(expr)) {
    return iterator;
  }
  if (type && type->isParam() && type->iterated()) {
    return TypeRelations(this->state).convert(expr, Type::iterator(type->iterated()));
  }
  if (type && type->isListParam()) {
    return TypeRelations(this->state)
        .convert(expr, Type::iterator(ArgListChecker(this->state).listElement(type)));
  }
  if (type && type->intlKind() == IntlKind::Segments) {
    return IntlChecker(this->state).segmentsArray(expr);
  }
  if (!type || (!type->isMap() && !type->isSet())) {
    return expr;
  }
  if (type->isMap()) {
    return this->dictCall(expr->loc(), expr, "entries", Builtin::MapEntries, {},
                          Type::iterator(Type::tuple({type->keyType(), type->valueType()})));
  }

  return this->dictCall(expr->loc(), expr, "values", Builtin::SetValues, {},
                        Type::iterator(type->keyType()));
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
  key = ExprChecker(this->state).checkExpr(key, Type::string());
  ResultTextKeyForRecord text = DictChecker::stringKey(key->type);
  if (text.isErr()) {
    this->state.getDiagnostics().error(key->loc(), text.error().message);
    return key;
  }

  std::string what = ErrorTextKeyType::keyLabel();

  return TypeRelations(this->state).coerce(key, Type::string(), what);
}

DictChecker::ResultDictMemberAccess DictChecker::checkDictMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::SMLoc loc = member->loc();
  if (type->isRecord()) {
    Expr *key = this->keyLiteral(loc, member->name);
    Expr *read = this->dictCall(loc, member->object, "get", Builtin::MapGet, {key},
                                Type::optional(type->valueType()));

    return ok(read);
  }
  if (member->name == "size") {
    Expr *size = this->dictCall(loc, member->object, "size", Builtin::MapSize, {}, Type::intTy());

    return ok(size);
  }
  std::string text = dictText(member->object, type);
  bool method = type->isMap() ? llvm::is_contained(kMapMethods, member->name)
                              : llvm::is_contained(kSetMethods, member->name);
  member->type = Type::error();
  if (method) {
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
    Expr *read = this->dictCall(index->loc(), index->object, "get", Builtin::MapGet, {key},
                                Type::optional(type->valueType()));

    return ok(read);
  }
  index->index = ExprChecker(this->state).checkExpr(index->index);
  std::string text = dictText(index->object, type);
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
  std::string text = dictText(object, type);
  if (!type->isRecord() && index) {
    index->index = ExprChecker(this->state).checkExpr(index->index);
  }
  if (type->isMap()) {
    std::string access = index ? std::string("[key]") : "." + member->name.str();
    std::string message = ErrorTextDictTarget::mapSet(text, access);

    return err(DictCheckerCheckDictTargetError{
        .code = DictCheckerCheckDictTargetErrorCode::MAP_TARGET,
        .message = message,
    });
  }
  if (!type->isRecord() && member && member->name == "size") {
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

  return this->dictCall(assign->loc(), object, "set", Builtin::RecordSet, {key, assign->value},
                        object->type->valueType());
}

Expr *DictChecker::checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index) {
  index->object = ExprChecker(this->state).checkExpr(index->object);
  const Type *type = index->object->type;
  if (!type->isRecord()) {
    return nullptr;
  }
  Expr *key = this->checkRecordKey(index->index);
  const Type *valueType = type->valueType();
  Flow before = this->state.getFlowState().getFlow();
  Expr *value = ExprChecker(this->state).checkExpr(assign->value, valueType);
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(before, this->state.getFlowState().getFlow()));
  bool definite = !value->type->isOptional() && !value->type->isUndefined();
  value = TypeRelations(this->state).coerce(value, valueType);

  return this->dictCall(assign->loc(), index->object, "set", Builtin::RecordNullishSet,
                        {key, value}, definite ? valueType->nonOptional() : valueType);
}

DictChecker::ResultRecordKeyDeleted
DictChecker::checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr, llvm::StringRef name) {
  const Type *type = object->type;
  unary->type = Type::boolTy();
  if (!type->isRecord()) {
    std::string text = dictText(object, type);
    if (keyExpr) {
      keyExpr = ExprChecker(this->state).checkExpr(keyExpr);
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
      unary->loc(), std::vector<Expr *>{this->dictCall(unary->loc(), object, "delete",
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
    std::string text = dictText(bin->rhs, type);
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
  key = TypeRelations(this->state).coerce(key, Type::string(), what);
  Expr *has = this->dictCall(bin->loc(), bin->rhs, "has", Builtin::MapHas, {key}, Type::boolTy());

  return ok(has);
}

}
