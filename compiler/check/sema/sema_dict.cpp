// Dictionaries (docs/objects.md, section 9): `Record<string, V>`, also
// written `{ [key: string]: V }`, which is read and written with the syntax
// of objects (`r[k]`, `r.k`, `delete r[k]`, `k in r`, Object.keys, for...in,
// spreads), and Map<K, V> and Set<T> with the methods of JavaScript. All
// three are one hash table of the runtime (runtime/map.c) that keeps its keys
// in the order they were added.
//
// Reading a key gives `V | undefined`, as reading an element at an index not
// proven in range does. The traps of JavaScript are errors: `m[k]` on a Map
// reads a property of the object rather than an entry, Object.keys of a Map is
// empty, and `Record<int, V>` would turn its keys into strings.
//
// The operations become calls of built-ins (Builtin::MapGet and the others
// after it), whose receiver is the object of the callee MemberExpr, so that
// CodeGen has them in one place (codegen_dict.cpp). Where values are
// iterated, a Map is its entries() and a Set its values(): iterators, which
// every consumer of iterators takes.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

std::u16string toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

// The name a dictionary goes by in messages: `r`, `m`, `s`, or the variable
// that holds it.
std::string dictText(const Expr *expr, const Type *type) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(expr))
    return name->name.str();
  if (auto *call = dyn_cast<CallExpr>(expr); call && call->builtin == Builtin::EnvMap)
    return "process.env";
  if (auto *member = dyn_cast<MemberExpr>(expr))
    if (auto *object = dyn_cast<NameExpr>(member->object))
      return (object->name + "." + member->name).str();
  return type->isRecord() ? "r" : type->isMap() ? "m" : "s";
}

// The methods of Map and Set, to tell a method read as a value from a
// property that does not exist.
const llvm::StringLiteral kMapMethods[] = {"get",  "set",    "has",     "delete", "clear",
                                           "keys", "values", "entries", "forEach"};
const llvm::StringLiteral kSetMethods[] = {"add",          "has",          "delete",
                                           "clear",        "keys",         "values",
                                           "entries",      "forEach",      "union",
                                           "intersection", "difference",   "symmetricDifference",
                                           "isSubsetOf",   "isSupersetOf", "isDisjointFrom"};

} // namespace

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

const Type *Sema::resolveDictType(const TypeRef &ref) {
  llvm::StringRef name = ref.name;
  if (name == "WeakRef")
    return resolveWeakRefType(ref);
  if (name == "WeakMap" || name == "WeakSet") {
    diags_.error(ref.loc, "'" + name + "' is not supported yet; a '" + name.drop_front(4).str() +
                              "' holds its keys, and frees them with itself");
    return Type::error();
  }
  bool record = name == "Record";
  bool map = name == "Map" || name == "ReadonlyMap";
  bool set = name == "Set" || name == "ReadonlySet";
  if (!record && !map && !set)
    return nullptr;
  size_t wanted = set ? 1 : 2;
  if (ref.kind != TypeRef::Kind::Generic || ref.args.size() != wanted) {
    diags_.error(ref.loc,
                 set   ? "'" + name + "' needs the type of its elements, e.g. '" + name + "<int>'"
                 : map ? "'" + name + "' needs the types of its keys and values, e.g. '" + name +
                             "<string, int>'"
                       : std::string("'Record' needs the types of its keys and values, "
                                     "e.g. 'Record<string, int>'"));
    return Type::error();
  }
  const Type *key = resolveType(ref.args[0], /*allowVoid=*/false);
  const Type *value = set ? Type::voidTy() : resolveType(ref.args[1], /*allowVoid=*/false);
  if (key->isError() || value->isError())
    return Type::error();
  if (record) {
    // `Record<"up" | "down", T>`, as in TypeScript: an object with exactly
    // those keys, each a T; no dictionary.
    llvm::ArrayRef<const Type *> literals =
        key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
    if (key->isStringLiteral() ||
        (key->isUnion() &&
         llvm::all_of(literals, [](const Type *m) { return m->isStringLiteral(); }))) {
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
      if (key->nonOptional()->isNumeric())
        diags_.error(ref.args[0].loc,
                     "the keys of a Record are strings: JavaScript would turn a number key into a "
                     "string, so 'r[1]' and 'r[\"1\"]' would be one; for numbers use a Map, e.g. "
                     "'Map<" +
                         key->name() + ", " + value->name() + ">'");
      else
        diags_.error(ref.args[0].loc, "the keys of a Record are strings, found '" + key->name() +
                                          "'; for other keys use a Map, e.g. 'Map<" + key->name() +
                                          ", " + value->name() + ">'");
      return Type::error();
    }
    return Type::dict(Type::DictKind::Record, key, value);
  }
  if (!checkKeyType(key, ref.args[0].loc))
    return Type::error();
  return Type::dict(map ? Type::DictKind::Map : Type::DictKind::Set, key, value);
}

bool Sema::checkKeyType(const Type *type, llvm::SMLoc loc) {
  if (type->isUnknown()) {
    diags_.error(loc, "an unknown cannot be a key: a key is found by its value, and what an "
                      "unknown holds is known only when the program runs; use keys of one type, "
                      "e.g. 'Map<string, unknown>'");
    return false;
  }
  if (type->nonOptional()->isResult()) {
    diags_.error(loc, "a Result cannot be a key: Results are not compared, as in JavaScript each "
                      "is an object of its own, equal to no other; use what it holds");
    return false;
  }
  return true;
}

Expr *Sema::dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                     std::vector<Expr *> args, const Type *type) {
  auto *member = ctx_.make<MemberExpr>(loc, receiver, name);
  member->type = type;
  auto *call = ctx_.make<CallExpr>(loc, member, std::move(args));
  call->builtin = builtin;
  call->type = type;
  return call;
}

Expr *Sema::iterableOf(Expr *expr) {
  const Type *type = expr->type;
  // An object with `[Symbol.iterator]()` (sema_iterable.cpp); in generic
  // code, `xs: Iterable<int>` as some iterator of ints.
  if (Expr *iterator = userIterator(expr))
    return iterator;
  if (type && type->isParam() && type->iterated())
    return convert(expr, Type::iterator(type->iterated()));
  // `args` of a list of types: some types.
  if (type && type->isListParam())
    return convert(expr, Type::iterator(listElement(type)));
  if (type && type->intlKind() == IntlKind::Segments)
    return segmentsArray(expr);
  if (!type || (!type->isMap() && !type->isSet()))
    return expr;
  if (type->isMap())
    return dictCall(expr->loc(), expr, "entries", Builtin::MapEntries, {},
                    Type::iterator(Type::tuple({type->keyType(), type->valueType()})));
  return dictCall(expr->loc(), expr, "values", Builtin::SetValues, {},
                  Type::iterator(type->keyType()));
}

Expr *Sema::keyLiteral(llvm::SMLoc loc, llvm::StringRef name) {
  auto *key = ctx_.make<StringLitExpr>(loc, toUtf16(name));
  key->type = Type::string();
  return key;
}

// The key of a Record, `r[key]`: a string.
Expr *Sema::checkRecordKey(Expr *key) {
  key = checkExpr(key, Type::string());
  const Type *type = key->type;
  if (type->nonOptional()->isNumeric()) {
    diags_.error(key->loc(), "the keys of a Record are strings, and JavaScript would turn this '" +
                                 type->name() +
                                 "' into one, so 1 and \"1\" would be one key; convert it, "
                                 "'String(i)', or use a Map, whose keys may be numbers");
    return key;
  }
  return coerce(key, Type::string(), "the key");
}

//===----------------------------------------------------------------------===//
// Reading and writing keys
//===----------------------------------------------------------------------===//

Expr *Sema::checkDictMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::SMLoc loc = member->loc();
  if (type->isRecord()) {
    // `r.key` is `r["key"]`; a key that names a method of Object.prototype
    // is a key like any other (docs/objects.md, section 9).
    Expr *key = keyLiteral(loc, member->name);
    return dictCall(loc, member->object, "get", Builtin::MapGet, {key},
                    Type::optional(type->valueType()));
  }
  if (member->name == "size")
    return dictCall(loc, member->object, "size", Builtin::MapSize, {}, Type::intTy());
  std::string text = dictText(member->object, type);
  bool method = type->isMap() ? llvm::is_contained(kMapMethods, member->name)
                              : llvm::is_contained(kSetMethods, member->name);
  if (method)
    diags_.error(loc, "'" + text + "." + member->name + "' can only be called, e.g. '" + text +
                          "." + member->name +
                          "(...)'; pass an arrow that calls it where a function is "
                          "expected");
  else
    diags_.error(loc,
                 "property '" + member->name + "' does not exist on type '" + type->name() + "'" +
                     (member->name == "length" ? "; the number of entries is '" + text + ".size'"
                                               : std::string()));
  member->type = Type::error();
  return member;
}

Expr *Sema::checkDictIndex(IndexExpr *index) {
  const Type *type = index->object->type;
  if (type->isRecord()) {
    Expr *key = checkRecordKey(index->index);
    return dictCall(index->loc(), index->object, "get", Builtin::MapGet, {key},
                    Type::optional(type->valueType()));
  }
  index->index = checkExpr(index->index);
  std::string text = dictText(index->object, type);
  if (type->isMap())
    diags_.error(index->loc(), "'" + text +
                                   "[key]' reads a property of the Map object in "
                                   "JavaScript, not one of its entries; read an entry with '" +
                                   text + ".get(key)'");
  else
    diags_.error(index->loc(), "a Set has no elements by position; check one with '" + text +
                                   ".has(value)', or copy them into an array, '[..." + text + "]'");
  index->type = Type::error();
  return index;
}

const Type *Sema::checkDictTarget(Expr *target, bool compound) {
  auto *index = dyn_cast<IndexExpr>(target);
  auto *member = dyn_cast<MemberExpr>(target);
  Expr *object = index ? index->object : member->object;
  const Type *type = object->type;
  target->type = Type::error();
  std::string text = dictText(object, type);
  if (!type->isRecord()) {
    if (index)
      index->index = checkExpr(index->index);
    if (type->isMap())
      diags_.error(target->loc(), "'" + text + (index ? "[key]" : "." + member->name.str()) +
                                      " = ...' sets a property of the Map object in JavaScript, "
                                      "not one of its entries; set an entry with '" +
                                      text + ".set(key, value)'");
    else if (member && member->name == "size")
      diags_.error(target->loc(), "the size of a Set cannot be assigned to; add elements with '" +
                                      text + ".add(value)'");
    else
      diags_.error(target->loc(),
                   "a Set has no elements by position; add one with '" + text + ".add(value)'");
    return Type::error();
  }
  if (index)
    index->index = checkRecordKey(index->index);
  if (compound) {
    std::string key = index ? text + "[k]" : text + "." + member->name.str();
    const Type *value = type->valueType();
    std::string hint = value->isNumeric() || value->isDecimal()
                           ? ", e.g. '" + key + " = (" + key + " ?? 0) + 1'"
                       : value->isString() ? ", e.g. '" + key + " = (" + key + " ?? \"\") + s'"
                                           : "";
    diags_.error(target->loc(), "'" + key +
                                    "' may be undefined, as a key may be missing: give it a "
                                    "default with '??'" +
                                    hint);
    return Type::error();
  }
  target->type = type->valueType();
  return type->valueType();
}

Expr *Sema::recordAssign(AssignExpr *assign) {
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  Expr *object = index ? index->object : member ? member->object : nullptr;
  if (!object || !object->type || !object->type->isRecord() || assign->op)
    return nullptr;
  Expr *key = index ? index->index : nullptr;
  if (!key)
    key = keyLiteral(member->loc(), member->name);
  return dictCall(assign->loc(), object, "set", Builtin::RecordSet, {key, assign->value},
                  object->type->valueType());
}

// `r[k] ??= v`: v is evaluated and stored only when r has no value at k.
Expr *Sema::checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index) {
  index->object = checkExpr(index->object);
  const Type *type = index->object->type;
  if (!type->isRecord())
    return nullptr;
  Expr *key = checkRecordKey(index->index);
  const Type *valueType = type->valueType();
  Flow before = flow_;
  Expr *value = checkExpr(assign->value, valueType);
  flow_ = join(before, flow_);
  bool definite = !value->type->isOptional() && !value->type->isUndefined();
  value = coerce(value, valueType);
  return dictCall(assign->loc(), index->object, "set", Builtin::RecordNullishSet, {key, value},
                  definite ? valueType->nonOptional() : valueType);
}

Expr *Sema::checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr, llvm::StringRef name) {
  const Type *type = object->type;
  unary->type = Type::boolTy();
  if (!type->isRecord()) {
    std::string text = dictText(object, type);
    if (keyExpr)
      keyExpr = checkExpr(keyExpr);
    diags_.error(unary->loc(), "'delete' removes a property of the " +
                                   std::string(type->isMap() ? "Map" : "Set") +
                                   " object in JavaScript, not an entry; remove one with '" + text +
                                   ".delete(" + (type->isMap() ? "key" : "value") + ")'");
    return unary;
  }
  Expr *key = keyExpr ? checkRecordKey(keyExpr) : keyLiteral(unary->loc(), name);
  // As in JavaScript, `delete` is true even for a key that was not there.
  auto *yes = ctx_.make<BoolLitExpr>(unary->loc(), true);
  yes->type = Type::boolTy();
  auto *seq = ctx_.make<SequenceExpr>(
      unary->loc(), std::vector<Expr *>{dictCall(unary->loc(), object, "delete", Builtin::MapDelete,
                                                 {key}, Type::boolTy()),
                                        yes});
  seq->type = Type::boolTy();
  return seq;
}

Expr *Sema::checkDictIn(BinaryExpr *bin) {
  const Type *type = bin->rhs->type->nonOptional();
  if (bin->rhs->type->isOptional()) {
    diags_.error(bin->rhs->loc(), "the dictionary may be undefined; check it first");
    return bin;
  }
  if (!type->isRecord()) {
    std::string text = dictText(bin->rhs, type);
    diags_.error(bin->loc(), "'in' asks for a property of the " +
                                 std::string(type->isMap() ? "Map" : "Set") +
                                 " object in JavaScript, not for an entry; ask with '" + text +
                                 ".has(" + (type->isMap() ? "key" : "value") + ")'");
    return bin;
  }
  Expr *key = bin->lhs;
  if (key->type->nonOptional()->isNumeric()) {
    diags_.error(key->loc(), "the keys of a Record are strings, and JavaScript would turn this '" +
                                 key->type->name() + "' into one; convert it, 'String(i)'");
    return bin;
  }
  key = coerce(key, Type::string(), "the key");
  return dictCall(bin->loc(), bin->rhs, "has", Builtin::MapHas, {key}, Type::boolTy());
}

//===----------------------------------------------------------------------===//
// Methods
//===----------------------------------------------------------------------===//

Expr *Sema::checkDictMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string text = dictText(member->object, type);
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  call->type = Type::error();
  if (type->isRecord()) {
    // A Record has no methods, only keys; a function stored at a key may be
    // missing.
    std::string key = text + "." + name.str();
    if (name == "hasOwnProperty" && call->args.size() == 1)
      diags_.error(member->loc(), "a Record inherits nothing from Object.prototype, and '" + key +
                                      "' is a key like any other; ask for a key with 'key in " +
                                      text + "'");
    else if (type->valueType()->isFunction())
      diags_.error(member->loc(), "'" + key +
                                      "' may be undefined, as a key may be missing; call it with "
                                      "'" +
                                      key + "?.(...)'");
    else
      diags_.error(member->loc(), "a Record has no methods, only keys, and '" + key + "' is a '" +
                                      type->valueType()->name() +
                                      " | undefined'; for the keys use 'Object.keys(" + text +
                                      ")'");
    checkArgsLoosely();
    return call;
  }
  const Type *key = type->keyType(), *value = type->valueType();
  bool isMap = type->isMap();
  auto set = [&](Builtin builtin, const Type *result) {
    call->builtin = builtin;
    call->type = result;
    return call;
  };
  auto arity = [&](unsigned min, unsigned max) {
    return checkArity(call, member->loc(), min, max);
  };
  auto keyArg = [&](size_t i) { call->args[i] = checkArg(call->args[i], key); };

  if (name == "get" && isMap) {
    if (!arity(1, 1))
      return call;
    keyArg(0);
    return set(Builtin::MapGet, Type::optional(value));
  }
  if (name == "set" && isMap) {
    if (!arity(2, 2))
      return call;
    keyArg(0);
    call->args[1] = checkArg(call->args[1], value);
    return set(Builtin::MapSet, type);
  }
  if (name == "add" && !isMap) {
    if (!arity(1, 1))
      return call;
    keyArg(0);
    return set(Builtin::SetAdd, type);
  }
  if (name == "has" || name == "delete") {
    if (!arity(1, 1))
      return call;
    keyArg(0);
    return set(name == "has" ? Builtin::MapHas : Builtin::MapDelete, Type::boolTy());
  }
  if (name == "clear") {
    if (!arity(0, 0))
      return call;
    return set(Builtin::MapClear, Type::voidTy());
  }
  if (name == "keys" || name == "values" || name == "entries") {
    if (!arity(0, 0))
      return call;
    if (!isMap)
      return name == "entries" ? set(Builtin::SetEntries, Type::iterator(Type::tuple({key, key})))
                               : set(Builtin::SetValues, Type::iterator(key));
    return name == "keys"     ? set(Builtin::MapKeys, Type::iterator(key))
           : name == "values" ? set(Builtin::MapValues, Type::iterator(value))
                              : set(Builtin::MapEntries, Type::iterator(Type::tuple({key, value})));
  }
  if (name == "forEach") {
    if (call->args.size() == 2) {
      diags_.error(call->args[1]->loc(),
                   "the second argument of 'forEach' sets 'this' for the callback, which is not "
                   "supported");
      checkArgsLoosely();
      return call;
    }
    if (!arity(1, 1))
      return call;
    // f(value, key, map), and for a Set f(value, value, set).
    Signature sig;
    sig.pad = false;
    sig.voidResult = true;
    sig.params = {isMap ? value : key, key, type};
    call->args[0] = checkFunctionArg(call->args[0], sig, isMap ? 2 : 1, "forEach");
    calledFunction(); // the callback may change anything
    return set(Builtin::MapForEach, Type::voidTy());
  }
  struct Combine {
    llvm::StringLiteral name;
    Builtin builtin;
    bool test;
  };
  static const Combine combines[] = {
      {"union", Builtin::SetUnion, false},
      {"intersection", Builtin::SetIntersection, false},
      {"difference", Builtin::SetDifference, false},
      {"symmetricDifference", Builtin::SetSymmetricDifference, false},
      {"isSubsetOf", Builtin::SetIsSubsetOf, true},
      {"isSupersetOf", Builtin::SetIsSupersetOf, true},
      {"isDisjointFrom", Builtin::SetIsDisjointFrom, true},
  };
  for (const Combine &c : combines) {
    if (isMap || c.name != name)
      continue;
    if (!arity(1, 1))
      return call;
    // Another set, or a map, whose keys are what it holds (a set-like
    // object of JavaScript).
    Expr *&other = call->args[0];
    other = checkExpr(other, type);
    const Type *otherType = other->type;
    if (otherType->isError())
      return call;
    if (!otherType->isDict() || otherType->isRecord() || otherType->keyType() != key) {
      diags_.error(other->loc(), "'" + name + "' needs a 'Set<" + key->name() + ">' or a 'Map<" +
                                     key->name() + ", ...>', found '" + otherType->name() +
                                     "'; make a Set first, e.g. 'new Set(xs)'");
      return call;
    }
    return set(c.builtin, c.test ? Type::boolTy() : type);
  }
  std::string message =
      "property '" + name.str() + "' does not exist on type '" + type->name().str() + "'";
  if (name == "length")
    message += "; the number of entries is '" + text + ".size'";
  else if (isMap && name == "add")
    message += "; a Map sets an entry with '" + text + ".set(key, value)'";
  else if (!isMap && (name == "set" || name == "get"))
    message += "; a Set adds a value with '" + text + ".add(value)' and checks one with '" + text +
               ".has(value)'";
  else if (name == "map" || name == "filter" || name == "reduce" || name == "some" ||
           name == "every" || name == "find")
    message += "; call it on an iterator, e.g. '" + text + (isMap ? ".entries()" : ".values()") +
               "." + name.str() + "(...)', or on an array, '[..." + text + "]." + name.str() +
               "(...)'";
  diags_.error(member->loc(), message);
  checkArgsLoosely();
  return call;
}

//===----------------------------------------------------------------------===//
// New dictionaries
//===----------------------------------------------------------------------===//

// `[["a", 1], ["b", 2]]`, where nothing gives the types of a Map: its pairs
// say them. Returns false if `arg` is no such literal.
bool Sema::inferPairs(Expr *arg, const Type *&key, const Type *&value) {
  auto *lit = dyn_cast<ArrayLitExpr>(arg);
  if (!lit || lit->elements.empty())
    return false;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = dyn_cast<ArrayLitExpr>(e.expr);
    if (e.spread || !pair || pair->elements.size() != 2 || pair->elements[0].spread ||
        pair->elements[1].spread)
      return false;
  }
  key = value = nullptr;
  bool ok = true;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = cast<ArrayLitExpr>(e.expr);
    for (size_t i = 0; i < 2; ++i) {
      Expr *&part = pair->elements[i].expr;
      const Type *&common = i == 0 ? key : value;
      part = checkExpr(part, common && isLiteralTree(part) ? common : nullptr);
      const Type *t = part->type;
      if (t->isError()) {
        ok = false;
        continue;
      }
      const Type *next = common ? unify(common, t) : t;
      if (!next) {
        diags_.error(part->loc(), std::string("the ") + (i == 0 ? "keys" : "values") +
                                      " of a Map have one type; found '" + common->name() +
                                      "' and '" + t->name() + "'");
        ok = false;
        continue;
      }
      common = next;
    }
  }
  if (!ok || !key || !value) {
    key = value = Type::error();
    return true;
  }
  if (key->isUndefined() || value->isUndefined() || key->isVoid() || value->isVoid()) {
    diags_.error(arg->loc(), "cannot infer the types of the Map from 'undefined'; give them, e.g. "
                             "'new Map<string, int | undefined>(...)'");
    key = value = Type::error();
    return true;
  }
  const Type *pairType = Type::tuple({key, value});
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = cast<ArrayLitExpr>(e.expr);
    pair->elements[0].expr = coerce(pair->elements[0].expr, key);
    pair->elements[1].expr = coerce(pair->elements[1].expr, value);
    pair->type = pairType;
  }
  lit->type = Type::array(pairType);
  return true;
}

// The entries a new Map takes ([key, value] pairs, with `pairs`), or the
// values of a new Set: whatever can be iterated. `element` is what each must
// be, null if nothing says yet; it becomes what each is. The result is the
// iterable: a Map or a Set as its iterator.
Expr *Sema::checkDictSource(Expr *arg, bool pairs, const Type *&element, llvm::StringRef what) {
  markFused(arg);
  arg = iterableOf(checkExpr(arg, element ? Type::array(element) : nullptr));
  const Type *type = arg->type;
  const Type *wanted = element;
  element = Type::error();
  if (type->isError())
    return arg;
  const Type *given = iteratedType(type);
  if (!given) {
    diags_.error(arg->loc(), what +
                                 " needs an array, a string, an iterator, a Map or a Set, found '" +
                                 type->name() + "'" +
                                 (type->isRecord() ? "; for the entries of a Record, pass "
                                                     "'Object.entries(r)'"
                                                   : ""));
    return arg;
  }
  if (type->isIterator()) {
    checkSendsNothing(type, arg->loc(), what);
    calledFunction(); // a generator runs
  }
  if (pairs) {
    bool pair = given->isTuple() && given->fields().size() == 2;
    if (!pair ||
        (wanted && (!isImplicitlyConvertible(given->fields()[0].type, wanted->fields()[0].type) ||
                    !isImplicitlyConvertible(given->fields()[1].type, wanted->fields()[1].type)))) {
      diags_.error(arg->loc(),
                   what + " takes [key, value] pairs" +
                       (wanted ? " of '" + wanted->name().str() + "'" : std::string()) +
                       ", found '" + given->name() + "'" +
                       (given->isArray() ? "; an array literal is a pair only where one "
                                           "is expected: give the callback its type, "
                                           "e.g. '(x): [string, int] => [x.name, x.n]'"
                                         : ""));
      return arg;
    }
  } else if (wanted && !isImplicitlyConvertible(given, wanted)) {
    diags_.error(arg->loc(), what + " takes values of type '" + wanted->name() + "', found '" +
                                 given->name() + "'");
    return arg;
  }
  element = given;
  return arg;
}

Expr *Sema::checkDictNew(NewExpr *expr, const Type *expected) {
  bool isMap = expr->name == "Map";
  llvm::SMLoc loc = expr->loc();
  if (expr->name == "WeakMap" || expr->name == "WeakSet") {
    TypeRef ref;
    ref.name = expr->name;
    ref.loc = loc;
    resolveDictType(ref); // reports it
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    expr->type = Type::error();
    return expr;
  }
  auto fail = [&] {
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    expr->type = Type::error();
    return expr;
  };
  const Type *type = nullptr;
  // Where a union is expected, its one Map (or Set), if it has one.
  if (expected && expected->nonOptional()->isUnion()) {
    const Type *only = nullptr;
    int count = 0;
    for (const Type *m : expected->nonOptional()->members())
      if (m->isDict() && !m->isRecord() && m->isMap() == isMap) {
        only = m;
        ++count;
      }
    expected = count == 1 ? only : nullptr;
  }
  if (!expr->typeArgs.empty()) {
    TypeRef ref;
    ref.kind = TypeRef::Kind::Generic;
    ref.name = expr->name;
    ref.loc = loc;
    ref.args = expr->typeArgs;
    type = resolveDictType(ref);
    if (type->isError())
      return fail();
  } else if (const Type *wanted = expected ? expected->nonOptional() : nullptr;
             wanted && wanted->isDict() && wanted->isMap() == isMap && !wanted->isRecord()) {
    type = wanted;
  }
  if (expr->args.size() > 1) {
    diags_.error(expr->args[1]->loc(), "'new " + expr->name + "' takes one argument, the " +
                                           (isMap ? "entries" : "values") + " to start with");
    return fail();
  }
  std::vector<Expr *> args;
  if (expr->args.empty()) {
    if (!type) {
      diags_.error(loc, isMap ? "'new Map()' needs the types of its keys and values, e.g. 'new "
                                "Map<string, int>()'"
                              : "'new Set()' needs the type of its elements, e.g. 'new "
                                "Set<int>()'");
      return fail();
    }
  } else if (Expr *arg = expr->args[0]; isa<UndefinedExpr>(arg)) {
    // `new Map(undefined)` starts empty, as JavaScript's does.
    if (!type) {
      diags_.error(
          loc, "'new " + expr->name + "(undefined)' needs the types of the " +
                   (isMap ? "Map, e.g. 'new Map<string, int>()'" : "Set, e.g. 'new Set<int>()'"));
      return fail();
    }
  } else {
    const Type *key = nullptr, *value = nullptr;
    if (!type && isMap && inferPairs(arg, key, value)) {
      if (key->isError() || !checkKeyType(key, arg->loc())) {
        expr->type = Type::error();
        return expr;
      }
      type = Type::dict(Type::DictKind::Map, key, value);
      args.push_back(arg);
    } else {
      const Type *element =
          type ? (isMap ? Type::tuple({type->keyType(), type->valueType()}) : type->keyType())
               : nullptr;
      arg = checkDictSource(arg, isMap, element, "'new " + expr->name.str() + "'");
      if (element->isError()) {
        expr->type = Type::error();
        return expr;
      }
      if (!type) {
        if (isMap && (!element->isTuple() || element->fields().size() != 2)) {
          diags_.error(arg->loc(),
                       "'new Map' takes [key, value] pairs, found '" + element->name() + "'");
          expr->type = Type::error();
          return expr;
        }
        const Type *k = isMap ? element->fields()[0].type : element;
        if (!checkKeyType(k, arg->loc())) {
          expr->type = Type::error();
          return expr;
        }
        if (k->isUndefined() || (isMap && element->fields()[1].type->isUndefined())) {
          diags_.error(arg->loc(), "cannot infer the types of the " + expr->name.str() +
                                       " from 'undefined'; give them");
          expr->type = Type::error();
          return expr;
        }
        type = isMap ? Type::dict(Type::DictKind::Map, k, element->fields()[1].type)
                     : Type::dict(Type::DictKind::Set, k, Type::voidTy());
      }
      args.push_back(arg);
    }
  }
  if (!checkKeyType(type->keyType(), loc))
    return fail();
  auto *call = ctx_.make<CallExpr>(loc, ctx_.make<NameExpr>(loc, expr->name), std::move(args));
  call->callee->type = type;
  call->builtin = Builtin::DictNew;
  call->type = type;
  return call;
}

// Object.fromEntries(entries): a Record of [key, value] pairs.
Expr *Sema::checkFromEntries(CallExpr *call, MemberExpr *member, const Type *expected) {
  if (rejectSpread(call) || !checkArity(call, member->loc(), 1, 1))
    return call;
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  const Type *type = wanted && wanted->isRecord() ? wanted : nullptr;
  const Type *element = type ? Type::tuple({Type::string(), type->valueType()}) : nullptr;
  const Type *key = nullptr, *value = nullptr;
  if (!type && inferPairs(call->args[0], key, value)) {
    // `[["a", 1], ["b", 2]]`: the pairs say the types.
    if (key->isError())
      return call;
    element = call->args[0]->type->inner();
  } else {
    call->args[0] = checkDictSource(call->args[0], /*pairs=*/true, element, "'Object.fromEntries'");
  }
  if (element->isError())
    return call;
  if (!type) {
    // The keys of an object are strings.
    if (!element->fields()[0].type->isString()) {
      diags_.error(call->args[0]->loc(),
                   "the keys of an object are strings, and JavaScript would turn '" +
                       element->fields()[0].type->name() +
                       "' keys into them; convert them first, or make a Map: 'new Map(...)'");
      return call;
    }
    const Type *values = element->fields()[1].type;
    if (values->isUndefined()) {
      diags_.error(call->args[0]->loc(), "cannot infer the type of the values from 'undefined'; "
                                         "declare it, e.g. 'const r: Record<string, int | "
                                         "undefined> = Object.fromEntries(...)'");
      return call;
    }
    type = Type::dict(Type::DictKind::Record, Type::string(), values);
  }
  call->builtin = Builtin::DictNew;
  call->type = type;
  return call;
}

// Map.groupBy(items, f) and Object.groupBy(items, f): the items in arrays, by
// what f gives for each.
Expr *Sema::checkGroupBy(CallExpr *call, MemberExpr *member, bool map) {
  std::string fn = map ? "Map.groupBy" : "Object.groupBy";
  if (rejectSpread(call) || !checkArity(call, member->loc(), 2, 2))
    return call;
  const Type *element = nullptr;
  call->args[0] = checkDictSource(call->args[0], /*pairs=*/false, element, "'" + fn + "'");
  if (element->isError()) {
    call->args[1] = checkExpr(call->args[1]);
    return call;
  }
  Signature sig;
  sig.pad = false;
  sig.params = {element, Type::intTy()};
  call->args[1] = checkFunctionArg(call->args[1], sig, 1, fn);
  calledFunction(); // the callback may change anything
  const Type *callback = call->args[1]->type;
  if (!callback->isFunction())
    return call;
  const Type *key = callback->ret();
  if (key->isVoid() || key->isUndefined()) {
    diags_.error(call->args[1]->loc(),
                 "the callback of '" + fn + "' must return the key of a group");
    return call;
  }
  if (!map && !key->isString()) {
    diags_.error(call->args[1]->loc(),
                 "the keys of an object are strings, and JavaScript would turn '" + key->name() +
                     "' keys into them; return a string, or group into a Map: 'Map.groupBy(...)'");
    return call;
  }
  if (!checkKeyType(key, call->args[1]->loc()))
    return call;
  call->builtin = Builtin::DictGroupBy;
  call->type =
      Type::dict(map ? Type::DictKind::Map : Type::DictKind::Record, key, Type::array(element));
  return call;
}

Expr *Sema::checkMapStatic(CallExpr *call, MemberExpr *member) {
  if (member->name == "groupBy")
    return checkGroupBy(call, member, /*map=*/true);
  diags_.error(member->loc(), "'Map." + member->name +
                                  "' is not supported; of Map only "
                                  "'Map.groupBy' and 'new Map' are");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

//===----------------------------------------------------------------------===//
// Object.keys and the like, for...in, literals
//===----------------------------------------------------------------------===//

Expr *Sema::checkDictObjectCall(CallExpr *call, llvm::StringRef fn) {
  Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (!type->isRecord()) {
    std::string text = dictText(arg, type);
    diags_.error(arg->loc(), "'Object." + fn + "' of a " + (type->isMap() ? "Map" : "Set") +
                                 " is always empty in JavaScript: its entries are no properties; "
                                 "use '[..." +
                                 text + "." + (fn == "entries" && type->isSet() ? "values" : fn) +
                                 "()]'");
    return call;
  }
  const Type *value = type->valueType();
  call->builtin = fn == "keys"     ? Builtin::ObjectKeys
                  : fn == "values" ? Builtin::ObjectValues
                                   : Builtin::ObjectEntries;
  call->type = fn == "keys"     ? Type::array(Type::string())
               : fn == "values" ? Type::array(value)
                                : Type::array(Type::tuple({Type::string(), value}));
  return call;
}

Expr *Sema::checkRecordLit(ObjectLitExpr *lit, const Type *record) {
  const Type *value = record->valueType();
  bool ok = true;
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.defaulted) {
      diags_.error(prop.loc, "'" + prop.name +
                                 " = ...' gives a default value, which only a "
                                 "destructuring pattern has; in an object literal "
                                 "write '" +
                                 prop.name + ": ...'");
      prop.value = cast<AssignExpr>(prop.value)->value;
      prop.defaulted = false;
      ok = false;
    }
    if (prop.method) {
      diags_.error(prop.loc, "a Record holds values, not methods; store a function, '" + prop.name +
                                 ": (...) => ...'");
      prop.value = checkExpr(prop.value);
      ok = false;
      continue;
    }
    if (prop.spread) {
      prop.value = checkExpr(prop.value, record);
      const Type *type = prop.value->type;
      if (type->isError()) {
        ok = false;
        continue;
      }
      if (!checkRecordSpread(prop, value))
        ok = false;
      continue;
    }
    if (prop.key)
      prop.key = checkRecordKey(prop.key);
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty())
      fe->fn->name = prop.name;
    prop.value = coerce(checkExpr(prop.value, value), value,
                        prop.key ? "the value" : ("key '" + prop.name + "'").str());
  }
  lit->type = ok ? record : Type::error();
  return lit;
}

bool Sema::checkRecordSpread(ObjectLitExpr::Property &prop, const Type *value) {
  const Type *type = prop.value->type;
  if (type->isOptional() && (type->inner()->isRecord() || type->inner()->isObject())) {
    diags_.error(prop.value->loc(), "the object may be undefined; check it before spreading it");
    return false;
  }
  if (!prop.omitKeys.empty()) {
    diags_.error(prop.loc, "the rest of a pattern that reads a key in brackets is not supported "
                           "for a Record; name the keys it leaves out");
    return false;
  }
  if (type->isRecord()) {
    if (!isImplicitlyConvertible(type->valueType(), value)) {
      diags_.error(prop.value->loc(),
                   "the values of '" + type->name() + "' are not '" + value->name() + "'");
      return false;
    }
    return true;
  }
  if (type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
      !type->isAnyObject()) {
    if (ClassDecl *cls = classOf(type))
      completeClass(cls);
    for (const Field &f : type->fields()) {
      if (llvm::StringRef(f.name).starts_with("#") || llvm::is_contained(prop.omit, f.name))
        continue;
      const Type *t = f.type->isOptional() && !value->isOptional() ? f.type->inner() : f.type;
      if (!isImplicitlyConvertible(t, value)) {
        diags_.error(prop.value->loc(), "field '" + f.name + "' of '" + type->name() + "' is '" +
                                            f.type->name() + "', and the values here are '" +
                                            value->name() + "'");
        return false;
      }
    }
    // Methods are no own properties of an instance in JavaScript, but the
    // methods of a literal and arrows in fields are.
    bool methods = false;
    for (const ClassDecl *c = classOf(type); c; c = c->parent)
      methods |=
          c->literal || llvm::any_of(c->fields, [](const ClassField &f) { return f.method; });
    if (methods) {
      diags_.error(prop.value->loc(), "an object with methods cannot be spread into a Record; copy "
                                      "its fields by name");
      return false;
    }
    return true;
  }
  std::string message =
      "only objects and Records can be spread into a Record, found '" + type->name().str() + "'";
  if (type->isMap() || type->isSet())
    message += "; in JavaScript a Map or a Set has no own properties, so this would add nothing";
  diags_.error(prop.value->loc(), message);
  return false;
}

Expr *Sema::recordFromSpreads(ObjectLitExpr *lit) {
  // The type of the values: of every named value, of the values of the
  // Records spread and of the fields of the objects spread.
  const Type *value = nullptr;
  bool ok = true;
  auto meet = [&](const Type *t, llvm::SMLoc loc) {
    if (!t || t->isError()) {
      ok = false;
      return;
    }
    const Type *next = value ? unify(value, t) : t;
    if (!next || next->isError()) {
      diags_.error(loc, "spreading a Record makes a Record, whose values have one type; found '" +
                            value->name() + "' and '" + t->name() +
                            "'; declare its type, e.g. 'const r: Record<string, " + value->name() +
                            "> = { ... }'");
      ok = false;
      return;
    }
    value = next;
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    const Type *type = prop.value->type;
    if (!prop.spread) {
      meet(type, prop.value->loc());
      continue;
    }
    if (type->isRecord())
      meet(type->valueType(), prop.value->loc());
    else if (type->isObject())
      for (const Field &f : type->fields())
        if (!llvm::StringRef(f.name).starts_with("#") && !llvm::is_contained(prop.omit, f.name))
          meet(f.type->isOptional() ? f.type->inner() : f.type, prop.value->loc());
  }
  if (!ok || !value) {
    lit->type = Type::error();
    return lit;
  }
  if (value->isUndefined() || value->isVoid()) {
    diags_.error(lit->loc(), "cannot infer the type of the values from 'undefined'; declare it, "
                             "e.g. 'const r: Record<string, int | undefined> = { ... }'");
    lit->type = Type::error();
    return lit;
  }
  const Type *record = Type::dict(Type::DictKind::Record, Type::string(), value);
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      prop.value = convert(prop.value, value);
      continue;
    }
    if (!checkRecordSpread(prop, value))
      ok = false;
  }
  lit->type = ok ? record : Type::error();
  return lit;
}

} // namespace tz
