#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/objects/dict_check_helpers/dict_check_helpers.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/whole_program/flows/flows.h"
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

DictChecker::DictChecker(Checker &owner)
    : checker(owner) {}

const Type *DictChecker::resolveDictType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (name == "WeakRef") {
    return this->checker.weakRefChecker().resolveWeakRefType(ref);
  }
  if (name == "WeakMap" || name == "WeakSet") {
    this->checker.diags_.error(ref.loc, "'" + name + "' is not supported yet; a '" +
                                            name.drop_front(4).str() +
                                            "' holds its keys, and frees them with itself");
    return Type::error();
  }
  bool record = name == "Record";
  bool map = name == "Map" || name == "ReadonlyMap";
  bool set = name == "Set" || name == "ReadonlySet";
  if (!record && !map && !set) {
    return nullptr;
  }
  size_t wanted = set ? 1 : 2;
  if (ref.kind() != TypeRefKind::GENERIC_REF || ref.args.size() != wanted) {
    this->checker.diags_.error(
        ref.loc, set   ? "'" + name + "' needs the type of its elements, e.g. '" + name + "<int>'"
                 : map ? "'" + name + "' needs the types of its keys and values, e.g. '" + name +
                             "<string, int>'"
                       : std::string("'Record' needs the types of its keys and values, "
                                     "e.g. 'Record<string, int>'"));
    return Type::error();
  }
  const Type *key = this->checker.typeResolver().resolveType(ref.args[0], false);
  const Type *value =
      set ? Type::voidTy() : this->checker.typeResolver().resolveType(ref.args[1], false);
  if (key->isError() || value->isError()) {
    return Type::error();
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
      return Type::object(std::move(fields));
    }
    if (!key->isString()) {
      if (key->nonOptional()->isNumeric()) {
        this->checker.diags_.error(
            ref.args[0].loc,
            "the keys of a Record are strings: JavaScript would turn a number key into a "
            "string, so 'r[1]' and 'r[\"1\"]' would be one; for numbers use a Map, e.g. "
            "'Map<" +
                key->name() + ", " + value->name() + ">'");
      } else {
        this->checker.diags_.error(ref.args[0].loc, "the keys of a Record are strings, found '" +
                                                        key->name() +
                                                        "'; for other keys use a Map, e.g. 'Map<" +
                                                        key->name() + ", " + value->name() + ">'");
      }
      return Type::error();
    }
    return Type::dict(Type::DictKind::Record, key, value);
  }
  if (!this->checkKeyType(key, ref.args[0].loc)) {
    return Type::error();
  }

  return Type::dict(map ? Type::DictKind::Map : Type::DictKind::Set, key, value);
}

bool DictChecker::checkKeyType(const Type *type, llvm::SMLoc loc) {
  if (type->isUnknown()) {
    this->checker.diags_.error(
        loc, "an unknown cannot be a key: a key is found by its value, and what an "
             "unknown holds is known only when the program runs; use keys of one type, "
             "e.g. 'Map<string, unknown>'");
    return false;
  }
  if (type->nonOptional()->isResult()) {
    this->checker.diags_.error(
        loc, "a Result cannot be a key: Results are not compared, as in JavaScript each "
             "is an object of its own, equal to no other; use what it holds");
    return false;
  }

  return true;
}

Expr *DictChecker::dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                            std::vector<Expr *> args, const Type *type) {
  auto *member = this->checker.ctx_.make<MemberExpr>(loc, receiver, name);
  member->type = type;
  auto *call = this->checker.ctx_.make<CallExpr>(loc, member, std::move(args));
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;

  return call;
}

Expr *DictChecker::iterableOf(Expr *expr) {
  const Type *type = expr->type;
  if (Expr *iterator = this->checker.iterableChecker().userIterator(expr)) {
    return iterator;
  }
  if (type && type->isParam() && type->iterated()) {
    return this->checker.typeRelations().convert(expr, Type::iterator(type->iterated()));
  }
  if (type && type->isListParam()) {
    return this->checker.typeRelations().convert(
        expr, Type::iterator(this->checker.argListChecker().listElement(type)));
  }
  if (type && type->intlKind() == IntlKind::Segments) {
    return this->checker.intlChecker().segmentsArray(expr);
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
  auto *key = this->checker.ctx_.make<StringLitExpr>(loc, DictChecker::toUtf16(name));
  key->type = Type::string();

  return key;
}

Expr *DictChecker::checkRecordKey(Expr *key) {
  key = this->checker.exprChecker().checkExpr(key, Type::string());
  const Type *type = key->type;
  if (type->nonOptional()->isNumeric()) {
    this->checker.diags_.error(
        key->loc(), "the keys of a Record are strings, and JavaScript would turn this '" +
                        type->name() +
                        "' into one, so 1 and \"1\" would be one key; convert it, "
                        "'String(i)', or use a Map, whose keys may be numbers");
    return key;
  }

  return this->checker.typeRelations().coerce(key, Type::string(), "the key");
}

Expr *DictChecker::checkDictMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::SMLoc loc = member->loc();
  if (type->isRecord()) {
    Expr *key = this->keyLiteral(loc, member->name);
    return this->dictCall(loc, member->object, "get", Builtin::MapGet, {key},
                          Type::optional(type->valueType()));
  }
  if (member->name == "size") {
    return this->dictCall(loc, member->object, "size", Builtin::MapSize, {}, Type::intTy());
  }
  std::string text = dictText(member->object, type);
  bool method = type->isMap() ? llvm::is_contained(kMapMethods, member->name)
                              : llvm::is_contained(kSetMethods, member->name);
  if (method) {
    this->checker.diags_.error(loc, "'" + text + "." + member->name +
                                        "' can only be called, e.g. '" + text + "." + member->name +
                                        "(...)'; pass an arrow that calls it where a function is "
                                        "expected");
  } else {
    this->checker.diags_.error(
        loc, "property '" + member->name + "' does not exist on type '" + type->name() + "'" +
                 (member->name == "length" ? "; the number of entries is '" + text + ".size'"
                                           : std::string()));
  }
  member->type = Type::error();

  return member;
}

Expr *DictChecker::checkDictIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  if (type->isRecord()) {
    Expr *key = this->checkRecordKey(index->index);
    return this->dictCall(index->loc(), index->object, "get", Builtin::MapGet, {key},
                          Type::optional(type->valueType()));
  }
  index->index = this->checker.exprChecker().checkExpr(index->index);
  std::string text = dictText(index->object, type);
  if (type->isMap()) {
    this->checker.diags_.error(index->loc(),
                               "'" + text +
                                   "[key]' reads a property of the Map object in "
                                   "JavaScript, not one of its entries; read an entry with '" +
                                   text + ".get(key)'");
  } else {
    this->checker.diags_.error(index->loc(),
                               "a Set has no elements by position; check one with '" + text +
                                   ".has(value)', or copy them into an array, '[..." + text + "]'");
  }
  index->type = Type::error();

  return index;
}

const Type *DictChecker::checkDictTarget(Expr *target, bool compound) {
  auto *index = dyn_cast<IndexExpr>(target);
  auto *member = dyn_cast<MemberExpr>(target);
  Expr *object = index ? index->object : member->object;
  const Type *type = object->type;
  target->type = Type::error();
  std::string text = dictText(object, type);
  if (!type->isRecord()) {
    if (index) {
      index->index = this->checker.exprChecker().checkExpr(index->index);
    }
    if (type->isMap()) {
      this->checker.diags_.error(target->loc(),
                                 "'" + text + (index ? "[key]" : "." + member->name.str()) +
                                     " = ...' sets a property of the Map object in JavaScript, "
                                     "not one of its entries; set an entry with '" +
                                     text + ".set(key, value)'");
    } else if (member && member->name == "size") {
      this->checker.diags_.error(target->loc(),
                                 "the size of a Set cannot be assigned to; add elements with '" +
                                     text + ".add(value)'");
    } else {
      this->checker.diags_.error(target->loc(),
                                 "a Set has no elements by position; add one with '" + text +
                                     ".add(value)'");
    }
    return Type::error();
  }
  if (index) {
    index->index = this->checkRecordKey(index->index);
  }
  if (compound) {
    std::string key = index ? text + "[k]" : text + "." + member->name.str();
    const Type *value = type->valueType();
    std::string hint = value->isNumeric() || value->isDecimal()
                           ? ", e.g. '" + key + " = (" + key + " ?? 0) + 1'"
                       : value->isString() ? ", e.g. '" + key + " = (" + key + " ?? \"\") + s'"
                                           : "";
    this->checker.diags_.error(target->loc(),
                               "'" + key +
                                   "' may be undefined, as a key may be missing: give it a "
                                   "default with '??'" +
                                   hint);
    return Type::error();
  }
  target->type = type->valueType();

  return type->valueType();
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
  index->object = this->checker.exprChecker().checkExpr(index->object);
  const Type *type = index->object->type;
  if (!type->isRecord()) {
    return nullptr;
  }
  Expr *key = this->checkRecordKey(index->index);
  const Type *valueType = type->valueType();
  Checker::Flow before = this->checker.flow_;
  Expr *value = this->checker.exprChecker().checkExpr(assign->value, valueType);
  this->checker.flow_ = FlowChecker::join(before, this->checker.flow_);
  bool definite = !value->type->isOptional() && !value->type->isUndefined();
  value = this->checker.typeRelations().coerce(value, valueType);

  return this->dictCall(assign->loc(), index->object, "set", Builtin::RecordNullishSet,
                        {key, value}, definite ? valueType->nonOptional() : valueType);
}

Expr *DictChecker::checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr,
                                   llvm::StringRef name) {
  const Type *type = object->type;
  unary->type = Type::boolTy();
  if (!type->isRecord()) {
    std::string text = dictText(object, type);
    if (keyExpr) {
      keyExpr = this->checker.exprChecker().checkExpr(keyExpr);
    }
    this->checker.diags_.error(unary->loc(),
                               "'delete' removes a property of the " +
                                   std::string(type->isMap() ? "Map" : "Set") +
                                   " object in JavaScript, not an entry; remove one with '" + text +
                                   ".delete(" + (type->isMap() ? "key" : "value") + ")'");
    return unary;
  }
  Expr *key = keyExpr ? this->checkRecordKey(keyExpr) : this->keyLiteral(unary->loc(), name);
  auto *yes = this->checker.ctx_.make<BoolLitExpr>(unary->loc(), true);
  yes->type = Type::boolTy();
  auto *seq = this->checker.ctx_.make<SequenceExpr>(
      unary->loc(), std::vector<Expr *>{this->dictCall(unary->loc(), object, "delete",
                                                       Builtin::MapDelete, {key}, Type::boolTy()),
                                        yes});
  seq->type = Type::boolTy();

  return seq;
}

Expr *DictChecker::checkDictIn(BinaryExpr *bin) {
  const Type *type = bin->rhs->type->nonOptional();
  if (bin->rhs->type->isOptional()) {
    this->checker.diags_.error(bin->rhs->loc(), "the dictionary may be undefined; check it first");
    return bin;
  }
  if (!type->isRecord()) {
    std::string text = dictText(bin->rhs, type);
    this->checker.diags_.error(bin->loc(),
                               "'in' asks for a property of the " +
                                   std::string(type->isMap() ? "Map" : "Set") +
                                   " object in JavaScript, not for an entry; ask with '" + text +
                                   ".has(" + (type->isMap() ? "key" : "value") + ")'");
    return bin;
  }
  Expr *key = bin->lhs;
  if (key->type->nonOptional()->isNumeric()) {
    this->checker.diags_.error(
        key->loc(), "the keys of a Record are strings, and JavaScript would turn this '" +
                        key->type->name() + "' into one; convert it, 'String(i)'");
    return bin;
  }
  key = this->checker.typeRelations().coerce(key, Type::string(), "the key");

  return this->dictCall(bin->loc(), bin->rhs, "has", Builtin::MapHas, {key}, Type::boolTy());
}

}
