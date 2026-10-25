// Objects: literals, spreads, fields and keys in brackets, tuples,
// Object.keys and the like, and the fields known to hold a value.
//
// Object types are exact: an object fits a type only with the same fields of
// the same types (see Type). A literal takes the type expected where it is
// written, `const p: Point = { x: 1, y: 2 }`, and then its values convert to
// the types of the fields, so x and y are doubles there; without an expected
// type the literal has the types of its values, `{ x: 1 }` is `{ x: int }`
// (unless where it goes later says otherwise, see sema_destination.cpp).
//
// A key in brackets, `{ [key]: v }` or `o[key]`, must be known when
// compiling, so that it names a field: objects have no dictionary of
// properties to look a name up in at run time.
//
// A field of type `T | undefined` that a check showed to hold a value is
// narrowed as a variable is (`if (q.limit != undefined) use(q.limit)`), as
// long as nothing may have changed it: Sema forgets it on any assignment to a
// field of that name in any object, on an assignment to the variable it is
// read through, and on a call of a function if the program assigns to a field
// of that name anywhere (any code may run in the call).

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// Whether an instance of the class has keys that are methods: a literal's
// methods, or arrows in fields of classes up the chain.
bool hasMethodFields(const ClassDecl *cls) {
  if (cls->literal)
    return true;
  for (const ClassDecl *c = cls; c; c = c->parent)
    if (llvm::any_of(c->fields, [](const ClassField &f) { return f.method; }))
      return true;
  return false;
}

// What JavaScript objects get from Object.prototype, which tz objects do not
// have.
bool isInherited(llvm::StringRef name) {
  static const char *const names[] = {"toString",      "valueOf",        "hasOwnProperty",
                                      "isPrototypeOf", "toLocaleString", "propertyIsEnumerable",
                                      "constructor",   "__proto__"};
  return llvm::is_contained(names, name);
}

std::string noField(const Type *type, llvm::StringRef name) {
  std::string message = ("property '" + name + "' does not exist on type '" + type->name() + "'").str();
  if (isInherited(name))
    message += "; objects inherit nothing from Object.prototype";
  return message;
}

// The expression a conversion that narrows a variable or a field converts:
// to the value of a `T | undefined`, or a variable to the class `instanceof`
// showed (`e.stdout` of `e` that is an ExecSyncError is a field of `e`).
const Expr *unnarrowed(const Expr *expr) {
  auto *conv = dyn_cast<CastExpr>(expr);
  if (!conv || !conv->isImplicit() || conv->shortCircuit)
    return expr;
  const Type *from = conv->operand->type;
  if (from->isOptional() && conv->type == from->inner())
    return conv->operand;
  if (isa<NameExpr>(conv->operand) && conv->type->isClass() && conv->type != from->nonOptional() &&
      conv->type->isSubtypeOf(from->nonOptional()))
    return conv->operand;
  return expr;
}

std::string toUtf8(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);
  return utf8;
}

// The tail of the messages about keys that are not constants.
const char kRuntimeKeys[] =
    "keys computed when the program runs need a dictionary: declare it as one, e.g. "
    "'const d: Record<string, int> = {}'";

} // namespace

std::string fieldText(const Expr *object, llvm::StringRef field) {
  std::string text = "o";
  if (auto *name = dyn_cast<NameExpr>(unnarrowed(object)))
    text = name->name.str();
  bool identifier = !field.empty() && !llvm::isDigit(field[0]) && llvm::all_of(field, [](char c) {
    return llvm::isAlnum(c) || c == '_' || c == '$';
  });
  return identifier ? text + "." + field.str() : text + "[\"" + field.str() + "\"]";
}

Expr *Sema::checkObjectLit(ObjectLitExpr *lit, const Type *expected) {
  if (expected && expected->nonOptional()->isJson())
    return checkJsonObjectLit(lit);
  if (expected && expected->nonOptional()->isRecord())
    return checkRecordLit(lit, expected->nonOptional());
  // A literal is never an instance of a class: where one is expected, the
  // literal keeps its own type, and the place reports the difference.
  const Type *target = expected ? expected->nonOptional() : nullptr;
  // Where a union is expected, the member the literal is written for (see
  // literalMember), once its keys are known.
  const Type *expectedUnion = target && target->isUnion() ? target : nullptr;
  auto usable = [](const Type *t) {
    return t && t->isObject() && !t->isIncomplete() && !t->isTuple() && !t->isClass() &&
           !t->isRegExp() && !t->isMatch() && !t->isDate() && !t->isIntl() &&
           !t->isBytes() && !t->isCrypto() && !t->isAnyObject();
  };
  if (!usable(target))
    target = nullptr;
  bool ok = true;

  // Whether each property names a field: a key in brackets may not.
  llvm::SmallVector<bool, 8> named(lit->props.size(), true);
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    named[i] = !prop.spread;
    if (prop.defaulted) {
      // `{ x = 1 }` belongs to destructuring patterns.
      diags_.error(prop.loc, "'" + prop.name +
                                 " = ...' gives a default value, which only a destructuring "
                                 "pattern such as '({ " +
                                 prop.name + " = 1 } = o)' has; in an object literal write '" +
                                 prop.name + ": ...'");
      prop.value = cast<AssignExpr>(prop.value)->value;
      prop.defaulted = false;
      ok = false;
    }
    if (!prop.key)
      continue;
    // `[key]: value`: the key must name the field when compiling.
    prop.key = checkExpr(prop.key);
    const Type *keyType = prop.key->type;
    if (std::optional<std::string> name = constantKey(prop.key)) {
      prop.name = ctx_.keepName(std::move(*name));
      file_->fieldNames.insert(prop.name);
      continue;
    }
    ok = false;
    named[i] = false;
    if (keyType->isError())
      continue;
    if (keyType->nonOptional()->isNumeric())
      diags_.error(prop.key->loc(),
                   "a number cannot name a field; an object with numbered entries is an array");
    else if (keyType->isString())
      diags_.error(prop.key->loc(),
                   std::string("a key in brackets must be known when compiling: a string "
                               "literal, or a constant declared with one, such as 'const key = "
                               "\"name\"'; ") +
                       kRuntimeKeys);
    else
      diags_.error(prop.key->loc(), "a key must be a string, found '" + keyType->name() + "'");
  }

  for (size_t i = 0; i < lit->props.size(); ++i)
    for (size_t j = 0; j < i && named[i]; ++j)
      if (named[j] && lit->props[j].name == lit->props[i].name) {
        diags_.error(lit->props[i].loc, "duplicate field '" + lit->props[i].name + "' in the object literal");
        ok = false;
        break;
      }
  if (expectedUnion) {
    target = literalMember(lit, expectedUnion);
    if (!usable(target))
      target = nullptr;
    // No object type of the union takes it: a Record of it does
    // (`Record<string, int> | int[]`).
    if (!target)
      for (const Type *m : expectedUnion->members())
        if (m->isRecord())
          return checkRecordLit(lit, m);
  }
  // A method where the type expected has a function that takes `this`,
  // `{ read() { this.push(x) } }` for `read?: (this: Readable, size: int) =>
  // void`, is that function: its `this` is what the caller gives.
  if (target)
    for (ObjectLitExpr::Property &prop : lit->props)
      if (const Field *wanted = prop.method ? target->field(prop.name) : nullptr;
          wanted && wanted->type->nonOptional()->isFunction() &&
          wanted->type->nonOptional()->hasThis())
        prop.method = false;
  if (llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.method; }))
    return checkMethodLiteral(lit, target);
  if (lit->hasSpread())
    return checkSpreadLit(lit, target, named, ok);

  std::vector<Field> fields;
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (!named[i]) { // a key in brackets that names nothing
      prop.value = checkExpr(prop.value);
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty())
      fe->fn->name = prop.name; // as JavaScript names it
    std::string what = ("field '" + prop.name + "'").str();
    if (target) {
      const Field *field = target->field(prop.name);
      if (!field) {
        prop.value = checkExpr(prop.value);
        if (!target->overload(prop.name)) // reported below
          diags_.error(prop.loc, "type '" + target->name() + "' has no field '" + prop.name +
                                     "'; an object must have exactly the fields of its type");
        continue;
      }
      prop.value = coerce(checkExpr(prop.value, field->type), field->type, what);
      continue;
    }
    prop.value = checkExpr(prop.value);
    const Type *type = prop.value->type;
    if (type->isError()) {
      ok = false;
    } else if (type->isVoid()) {
      diags_.error(prop.value->loc(), "an expression of type 'void' cannot be the value of " + what);
      ok = false;
    } else if (type->isUndefined()) {
      diags_.error(prop.value->loc(), "cannot infer the type of " + what +
                                          " from 'undefined'; declare the type of the object, e.g. "
                                          "'const o: { " + prop.name + "?: int } = ...'");
      ok = false;
    }
    fields.push_back({prop.name.str(), type, false});
  }

  if (target) {
    for (const Field &f : target->fields()) {
      bool given = llvm::any_of(lit->props, [&](const ObjectLitExpr::Property &p) { return p.name == f.name; });
      if (!given && !f.type->isOptional())
        diags_.error(lit->loc(), "field '" + f.name + "' is missing; type '" + target->name() + "' requires it");
    }
    for (const Overload &o : target->overloads())
      diags_.error(lit->loc(), "an object literal cannot be of type '" + target->name() +
                                   "': its method '" + o.name +
                                   "' has several signatures, and only a class implements "
                                   "those, each with a variant of its own");
    lit->type = target;
    return lit;
  }
  lit->type = ok ? Type::object(std::move(fields)) : Type::error();
  return lit;
}

// `{ ...a, x: 1, ...b }`: the fields of its parts, in order. A later part
// replaces a field an earlier one gave, except that an optional field that
// holds undefined is not copied, so `{ ...defaults, ...options }` keeps the
// defaults that the options leave out. The field then holds a value if any
// part gave it one.
Expr *Sema::checkSpreadLit(ObjectLitExpr *lit, const Type *target, llvm::ArrayRef<bool> named,
                           bool ok) {
  // Spreading a Record makes a Record, as its keys are known only when the
  // program runs.
  bool records = false;
  std::vector<Field> fields;
  auto slot = [&](llvm::StringRef name) -> Field * {
    for (Field &f : fields)
      if (f.name == name)
        return &f;
    return nullptr;
  };
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (!prop.spread) {
      if (!named[i]) {
        prop.value = checkExpr(prop.value);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty())
        fe->fn->name = prop.name;
      const Field *wanted = target ? target->field(prop.name) : nullptr;
      prop.value = checkExpr(prop.value, wanted ? wanted->type : nullptr);
      const Type *type = prop.value->type;
      Field *field = slot(prop.name);
      std::string what = ("field '" + prop.name + "'").str();
      if (type->isError()) {
        ok = false;
        continue;
      }
      if (type->isVoid()) {
        diags_.error(prop.value->loc(),
                     "an expression of type 'void' cannot be the value of " + what);
        ok = false;
        continue;
      }
      if (type->isUndefined()) {
        if (!field) {
          diags_.error(prop.value->loc(),
                       "cannot infer the type of " + what +
                           " from 'undefined'; declare the type of the object, e.g. "
                           "'const o: { " +
                           prop.name + "?: int } = ...'");
          ok = false;
          continue;
        }
        type = Type::optional(field->type);
      }
      if (field)
        field->type = type;
      else
        fields.push_back({prop.name.str(), type, false});
      continue;
    }

    prop.value = checkExpr(prop.value);
    const Type *type = prop.value->type;
    if (type->isError()) {
      ok = false;
      continue;
    }
    if (type->isRecord() && !target) {
      records = true;
      continue;
    }
    if (type->isOptional() && type->inner()->isObject() && !type->inner()->isTuple()) {
      diags_.error(prop.value->loc(), "the object may be undefined; check it before spreading it");
      ok = false;
      continue;
    }
    if (!type->isObject() || type->isTuple() || type->isRegExp() || type->isMatch() ||
        type->isDate() || type->isIntl() || type->isBytes() || type->isCrypto()) {
      std::string message =
          "only objects can be spread into an object literal, found '" + type->name().str() + "'";
      if (type->nonOptional()->isArray() || type->nonOptional()->isTuple() || type->isString())
        message += "; JavaScript would make its indices the keys";
      diags_.error(prop.value->loc(), message);
      ok = false;
      continue;
    }
    // The rest of a pattern leaves out the fields the pattern names, also
    // those named by keys in brackets, which are known by now. An instance
    // of a class keeps its `#private` fields, as in JavaScript.
    for (const IndexExpr *read : prop.omitKeys)
      if (!read->field.empty())
        prop.omit.push_back(read->field);
    prop.omitKeys.clear();
    if (ClassDecl *cls = classOf(type)) {
      completeClass(cls);
      for (const Field &f : type->fields())
        if (llvm::StringRef(f.name).starts_with("#"))
          prop.omit.push_back(ctx_.keepName(f.name));
      if (hasMethodFields(cls)) {
        diags_.error(prop.value->loc(), "an object with methods cannot be spread into an object "
                                        "literal yet; copy the fields by name");
        ok = false;
        continue;
      }
    }
    for (const Field &f : type->fields()) {
      if (llvm::is_contained(prop.omit, f.name))
        continue;
      Field *field = slot(f.name);
      if (!field) {
        fields.push_back({f.name, f.type, false});
        continue;
      }
      if (!f.type->isOptional()) {
        field->type = f.type;
        continue;
      }
      // Undefined is not copied: the earlier value may stay.
      const Type *base = unify(field->type->nonOptional(), f.type->inner());
      if (!base || base->isError()) {
        diags_.error(prop.loc, "field '" + f.name + "' is '" + field->type->name() +
                                   "' before the spread and '" + f.type->name() + "' in '" +
                                   type->name() + "'; a field has one type");
        ok = false;
        continue;
      }
      field->type = field->type->isOptional() ? Type::optional(base) : base;
    }
  }
  if (records && ok)
    return recordFromSpreads(lit);
  if (!ok) {
    lit->type = Type::error();
    return lit;
  }

  // An expected type with these fields gives the object its type; otherwise
  // the place it goes reports the difference.
  const Type *result = Type::object(fields);
  if (target) {
    bool fits = llvm::all_of(fields, [&](const Field &f) {
      const Field *t = target->field(f.name);
      return t && isImplicitlyConvertible(f.type, t->type);
    });
    fits &= llvm::all_of(target->fields(),
                         [&](const Field &t) { return t.type->isOptional() || slot(t.name); });
    if (fits)
      result = target;
  }

  // The values convert to the types of their fields as they are stored; a
  // value that a later part surely replaces is only evaluated.
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    auto replacedLater = [&](llvm::StringRef name) {
      for (size_t j = i + 1; j < lit->props.size(); ++j) {
        const ObjectLitExpr::Property &later = lit->props[j];
        if (!later.spread) {
          if (later.name == name)
            return true;
        } else if (!llvm::is_contained(later.omit, name)) {
          if (const Field *f = later.value->type->field(name); f && !f->type->isOptional())
            return true;
        }
      }
      return false;
    };
    if (!prop.spread) {
      if (replacedLater(prop.name))
        prop.replaced = true;
      else
        prop.value = convert(prop.value, result->field(prop.name)->type);
      continue;
    }
    for (const Field &f : prop.value->type->fields())
      if (!llvm::is_contained(prop.omit, f.name) && replacedLater(f.name))
        prop.replacedFields.push_back(f.name);
  }
  lit->type = result;
  return lit;
}

std::optional<std::string> Sema::constantKey(const Expr *key) const {
  if (auto *lit = dyn_cast<StringLitExpr>(key))
    return toUtf8(lit->value);
  if (auto *tmpl = dyn_cast<TemplateExpr>(key); tmpl && tmpl->exprs.empty())
    return toUtf8(tmpl->quasis[0]);
  if (auto *name = dyn_cast<NameExpr>(key); name && name->var)
    if (auto found = constKeys_.find(name->var); found != constKeys_.end())
      return found->second;
  return std::nullopt;
}

void Sema::keyNames(const Expr *key, llvm::function_ref<void(llvm::StringRef)> each) const {
  if (auto *lit = dyn_cast<StringLitExpr>(key)) {
    each(toUtf8(lit->value));
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(key); tmpl && tmpl->exprs.empty()) {
    each(toUtf8(tmpl->quasis[0]));
  } else if (auto *number = dyn_cast<IntLitExpr>(key)) {
    each(std::to_string(number->value)); // an element of a tuple
  } else if (auto *name = dyn_cast<NameExpr>(key)) {
    for (const std::string &text : file_->stringConstants.lookup(name->name))
      each(text);
  } else if (auto *member = dyn_cast<MemberExpr>(key)) {
    if (auto *object = dyn_cast<NameExpr>(member->object))
      for (const std::string &text :
           file_->stringConstants.lookup((object->name + "." + member->name).str()))
        each(text);
  }
}

const Type *Sema::keyedFieldType(const Type *type, const Type *keyType) {
  if (type->isTuple() || !type->isObject())
    return nullptr;
  // Of a class, its public fields, which lie at the same place in the
  // objects of the classes that extend it (`K extends keyof T` with a class).
  const ClassDecl *cls = type->isClass() ? classOf(type) : nullptr;
  if (type->isClass() && !cls)
    return nullptr;
  llvm::ArrayRef<const Type *> names =
      keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
  const Type *common = nullptr;
  for (const Type *n : names) {
    if (!n->isStringLiteral())
      return nullptr;
    std::string text = toUtf8(n->literalText());
    const Field *f = type->field(text);
    if (cls) {
      const ClassField *field = findField(cls, text, nullptr);
      if (!field || field->isStatic || field->access != Access::Public)
        return nullptr;
    }
    if (!f || (common && f->type != common))
      return nullptr;
    common = f->type;
  }
  return common;
}

Expr *Sema::checkKeyedField(IndexExpr *index, bool write) {
  const Type *type = index->object->type;
  index->type = Type::error();
  std::string name;
  if (type->isTuple()) {
    // Each element has its own type, so the position must be a constant.
    index->index = checkExpr(index->index, Type::intTy());
    const Type *keyType = index->index->type;
    if (keyType->isError())
      return index;
    std::optional<int64_t> position =
        keyType->isInteger() ? intConstant(index->index) : std::nullopt;
    size_t count = type->fields().size();
    if (!keyType->isInteger()) {
      diags_.error(index->index->loc(),
                   "a tuple is indexed by a number, found '" + keyType->name() + "'");
      return index;
    }
    if (!position) {
      diags_.error(index->loc(),
                   "a tuple can be indexed only by a constant, such as '[0]': each element has "
                   "its own type; or destructure it: 'const [a, b] = t'");
      return index;
    }
    if (*position < 0 || uint64_t(*position) >= count) {
      diags_.error(index->index->loc(), "tuple '" + type->name() + "' has " +
                                            std::to_string(count) + " element" +
                                            (count == 1 ? "" : "s") + "; there is no element " +
                                            std::to_string(*position));
      return index;
    }
    name = std::to_string(*position);
  } else {
    index->index = checkExpr(index->index);
    const Type *keyType = index->index->type;
    if (keyType->isError())
      return index;
    std::optional<std::string> key = constantKey(index->index);
    // A key of literals, each naming a field of one type: `counts[f.kind]`.
    if (const Type *field = key ? nullptr : keyedFieldType(type, keyType)) {
      llvm::ArrayRef<const Type *> names =
          keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
      if (write) {
        // What Sema knows of a field (narrowed, defined) it learns from
        // assignments by name.
        if (field->isOptional() || field->isUnion() || field->isLiteral() ||
            llvm::any_of(names, [&](const Type *n) {
              return type->field(toUtf8(n->literalText()))->readonly;
            })) {
          diags_.error(index->loc(), "fields of type '" + field->name() +
                                         "' are assigned by name, as in 'o.name = v'; through a "
                                         "key of literals only fields of a plain type are");
          return index;
        }
        for (const Type *n : names)
          fieldsAssigned(ctx_.keepName(toUtf8(n->literalText())));
      }
      index->keyed = true;
      index->type = field;
      return index;
    }
    if (!key) {
      if (keyType->nonOptional()->isNumeric())
        diags_.error(index->loc(), "type '" + type->name() +
                                       "' has no numbered fields; read a field by its "
                                       "name, e.g. 'o.name'");
      else if (keyType->isString())
        diags_.error(index->loc(),
                     "type '" + type->name() +
                         "' can be indexed only by a key known when compiling: a string literal, "
                         "or a constant declared with one, such as 'const key = \"name\"'; " +
                         kRuntimeKeys);
      else
        diags_.error(index->index->loc(),
                     "a key must be a string, found '" + keyType->name() + "'");
      return index;
    }
    if (!type->isClass() && !type->field(*key)) {
      diags_.error(index->loc(), noField(type, *key));
      return index;
    }
    name = std::move(*key);
  }
  // The program is scanned for the fields it assigns before it is checked
  // (see keyNames): a key it could not tell there cannot be written through.
  if (write && !assignedFields_.contains(name)) {
    diags_.error(index->index->loc(),
                 type->isTuple() ? "an element of a tuple is assigned by its number: 't[0] = ...'"
                                 : "a field assigned through a key in brackets must be "
                                   "named by a string literal, or a constant declared with "
                                   "one, such as 'const key = \"name\"'");
    return index;
  }
  index->field = ctx_.keepName(name);
  auto *member = ctx_.make<MemberExpr>(index->loc(), index->object, index->field);
  if (write)
    return member;
  return checkField(member);
}

//===----------------------------------------------------------------------===//
// Tuples
//===----------------------------------------------------------------------===//

const Type *Sema::tupleElement(const Type *tuple) {
  const Type *element = nullptr;
  for (const Field &f : tuple->fields()) {
    element = element ? unionUnify(element, f.type) : f.type;
    if (!element || element->isError())
      return nullptr;
  }
  return element;
}

// `t.length` is known when compiling; the tuple is still evaluated.
Expr *Sema::checkTupleMember(MemberExpr *member) {
  const Type *type = member->object->type;
  if (member->name != "length") {
    diags_.error(member->loc(),
                 "property '" + member->name + "' does not exist on type '" + type->name() + "'");
    member->type = Type::error();
    return member;
  }
  auto *length = ctx_.make<IntLitExpr>(member->loc(), type->fields().size());
  length->type = Type::intTy();
  if (isa<NameExpr>(member->object))
    return length;
  auto *seq = ctx_.make<SequenceExpr>(member->loc(), std::vector<Expr *>{member->object, length});
  seq->type = Type::intTy();
  return seq;
}

Expr *Sema::checkTupleMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string text = "t";
  if (auto *object = dyn_cast<NameExpr>(unnarrowed(member->object)))
    text = object->name.str();
  static const char *const resizing[] = {"push", "pop", "shift", "unshift", "splice"};
  if (name == "join")
    return checkTupleJoin(call, member);
  if (llvm::is_contained(resizing, name))
    diags_.error(member->loc(),
                 "a tuple has a fixed length, and '" + name +
                     "' would change it; to change the length, copy it into an array: '[..." +
                     text + "]'");
  else if (tupleElement(type))
    diags_.error(member->loc(),
                 "'" + name +
                     "' is not supported on tuples; copy the tuple into an array first: '[..." +
                     text + "]." + name + "(...)'");
  else
    diags_.error(member->loc(),
                 "'" + name +
                     "' is not supported on tuples; destructure it instead, e.g. 'const [a, b] = " +
                     text + "'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  call->type = Type::error();
  return call;
}

// `["Ann", 30]` where `[string, int]` is expected: a value for each place.
Expr *Sema::checkTupleLit(ArrayLitExpr *lit, const Type *tuple) {
  llvm::ArrayRef<Field> fields = tuple->fields();
  lit->type = Type::error();
  auto spread =
      llvm::find_if(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
  if (spread != lit->elements.end() || lit->elements.size() != fields.size()) {
    if (spread != lit->elements.end())
      diags_.error(spread->expr->loc(), "a tuple cannot take spread values: type '" +
                                            tuple->name() +
                                            "' has a fixed length; write each element");
    else
      diags_.error(lit->loc(), "type '" + tuple->name() + "' has " + std::to_string(fields.size()) +
                                   " elements, and this literal has " +
                                   std::to_string(lit->elements.size()));
    for (ArrayLitExpr::Element &e : lit->elements)
      e.expr = checkExpr(e.expr);
    return lit;
  }
  for (size_t i = 0; i < fields.size(); ++i) {
    Expr *&e = lit->elements[i].expr;
    e = coerce(checkExpr(e, fields[i].type), fields[i].type, "element " + std::to_string(i));
  }
  lit->type = tuple;
  return lit;
}

//===----------------------------------------------------------------------===//
// Object.keys, Object.values, Object.entries and for...in
//===----------------------------------------------------------------------===//

// The fields that hold a value, in the order the object got them, as the
// object prints (see TzLayout in runtime/tzrt.h).
Expr *Sema::checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef fn = member->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (fn == "groupBy")
    return checkGroupBy(call, member, /*map=*/false);
  if (fn == "fromEntries")
    return checkFromEntries(call, member, expected);
  if (fn != "keys" && fn != "values" && fn != "entries") {
    diags_.error(member->loc(), "'Object." + fn +
                                    "' is not supported; only Object.keys, Object.values, "
                                    "Object.entries, Object.fromEntries and Object.groupBy are");
    checkArgsLoosely();
    return call;
  }
  if (rejectSpread(call) || !checkArity(call, member->loc(), 1, 1))
    return call;
  Expr *&arg = call->args[0];
  arg = checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError())
    return call;
  // The keys of a Json, as JavaScript gives them for any value.
  if (type->nonOptional()->isJson()) {
    call->builtin = fn == "keys" ? Builtin::ObjectKeys : fn == "values" ? Builtin::ObjectValues
                                                                         : Builtin::ObjectEntries;
    const Type *json = Type::json();
    call->type = fn == "keys"     ? Type::array(Type::string())
                 : fn == "values" ? Type::array(json)
                                  : Type::array(Type::tuple({Type::string(), json}));
    return call;
  }
  if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
    diags_.error(arg->loc(), "the object may be undefined; check it first");
    return call;
  }
  if (type->isDict())
    return checkDictObjectCall(call, fn);
  if (!type->isObject() || type->isTuple() || type->isRegExp() || type->isMatch() ||
      type->isDate() || type->isIntl() || type->isBytes() || type->isCrypto()) {
    diags_.error(arg->loc(), "'Object." + fn + "' needs an object, found '" + type->name() + "'");
    return call;
  }
  if (fn == "keys") {
    call->builtin = Builtin::ObjectKeys;
    call->type = Type::array(Type::string());
    return call;
  }
  if (ClassDecl *cls = classOf(type)) {
    completeClass(cls);
    // Its first keys are node's _events and the others (sema_events.cpp),
    // which the program cannot read.
    if (isEmitterClass(cls)) {
      diags_.error(arg->loc(), "'Object." + fn + "' of an emitter is not supported: its keys are "
                               "node's _events, _eventsCount and _maxListeners; read the fields "
                               "of its class by name");
      return call;
    }
    if (hasMethodFields(cls)) {
      diags_.error(arg->loc(), "'Object." + fn + "' of an object with methods is not supported; "
                               "read the fields by name");
      return call;
    }
  }
  // One array holds the values, of the union of the types of the fields
  // (docs/objects.md, section 10). A field that does not hold a value is
  // left out, so `limit?: int` gives an int. The object may be of a type
  // down the declared chain, whose fields count too.
  const Type *element = nullptr;
  std::vector<std::pair<const Type *, const Field *>> fields;
  for (const Field &f : type->fields())
    fields.push_back({type, &f});
  for (const Type *t : file_->subtypes) {
    if (t == type || !t->isSubtypeOf(type))
      continue;
    if (ClassDecl *sub = classOf(t))
      completeClass(sub);
    for (const Field &f : t->fields())
      fields.push_back({t, &f});
  }
  for (auto [t0, field] : fields) {
    const Field &f = *field;
    if (t0->isClass() && llvm::StringRef(f.name).starts_with("#"))
      continue; // not a key in JavaScript
    const Type *t = f.type->nonOptional();
    const Type *both = element ? Type::unionOf({element, t}) : t;
    if (std::string problem = unionProblem(both); !problem.empty()) {
      diags_.error(arg->loc(), "'Object." + fn +
                                   "' puts the values of all the fields in one array, of the "
                                   "union of their types, and the fields of '" +
                                   t0->name() + "' have types that no union holds: " + problem +
                                   "; read the fields by name");
      return call;
    }
    element = both;
  }
  if (!element) {
    diags_.error(arg->loc(), "'" + type->name() + "' has no fields, so 'Object." + fn +
                                 "' would always be empty");
    return call;
  }
  if (fn == "values") {
    call->builtin = Builtin::ObjectValues;
    call->type = Type::array(element);
  } else {
    call->builtin = Builtin::ObjectEntries;
    call->type = Type::array(Type::tuple({Type::string(), element}));
  }
  return call;
}

// `for (const k in o)` walks Object.keys(o). Over an array JavaScript gives
// the indices as strings, which is a trap: `i + 1` is "01".
void Sema::checkForIn(ForOfStmt *stmt) {
  // A variable, or process.env, which can be read again in the body.
  auto *variable = dyn_cast<NameExpr>(stmt->iterable);
  bool env = isProcessMember(stmt->iterable, "env");
  Expr *object = checkExpr(stmt->iterable);
  const Type *type = object->type;
  // As V8 does, a key deleted while the loop runs is not visited: the body
  // starts with `if (!(k in r)) continue`.
  if (type->isRecord() && (variable || env) && !isa<VarDeclStmt>(stmt->body) &&
      !isa<FunctionDecl>(stmt->body)) {
    llvm::SMLoc loc = stmt->body->loc();
    Expr *again = variable ? static_cast<Expr *>(ctx_.make<NameExpr>(loc, variable->name))
                           : ctx_.make<MemberExpr>(loc, ctx_.make<NameExpr>(loc, "process"), "env");
    auto *has = ctx_.make<BinaryExpr>(loc, BinaryOp::In,
                                      ctx_.make<NameExpr>(loc, stmt->binding.name), again);
    auto *skip = ctx_.make<IfStmt>(loc, ctx_.make<UnaryExpr>(loc, UnaryOp::Not, has),
                                   ctx_.make<ContinueStmt>(loc, ""), nullptr);
    auto *block = dyn_cast<BlockStmt>(stmt->body);
    if (!block) {
      block = ctx_.make<BlockStmt>(loc);
      block->body.push_back(stmt->body);
      stmt->body = block;
    }
    block->body.insert(block->body.begin(), skip);
  }
  std::string text = type->isString() ? "s" : "a";
  if (auto *name = dyn_cast<NameExpr>(unnarrowed(object)))
    text = name->name.str();
  bool ok = false;
  if (type->isArray() || type->isString() || type->isTuple()) {
    const char *what = type->isArray() ? "an array" : type->isString() ? "a string" : "a tuple";
    diags_.error(object->loc(),
                 std::string("'for...in' over ") + what +
                     " gives its indices as strings ('0', '1'...), so 'i + 1' would be '01'; walk "
                     "the elements with 'for (const x of " +
                     text + ")', or the indices with 'for (let i = 0; i < " + text +
                     ".length; i++)'" +
                     (type->isArray() ? " or '" + text + ".forEach((x, i) => ...)'" : ""));
  } else if (type->isOptional() && type->inner()->isObject()) {
    diags_.error(object->loc(), "the object may be undefined; check it before walking its keys");
  } else if (type->nonOptional()->isJson()) {
    ok = true; // the keys of a Json object (see checkObjectCall)
  } else if (type->isRecord()) {
    ok = true;
  } else if (type->isDict()) {
    diags_.error(object->loc(),
                 std::string("'for...in' walks the properties of an object, and a ") +
                     (type->isMap() ? "Map" : "Set") +
                     " has none in JavaScript; walk its " + (type->isMap() ? "keys" : "values") +
                     " with 'for (const x of " + text + (type->isMap() ? ".keys()" : "") + ")'");
  } else if (type->isOptional() && type->inner()->isRecord()) {
    diags_.error(object->loc(), "the object may be undefined; check it before walking its keys");
  } else if ((!type->isObject() || type->isRegExp() || type->isMatch() || type->isDate() ||
              type->isIntl() || type->isBytes() || type->isCrypto()) &&
             !type->isError()) {
    diags_.error(object->loc(),
                 "'for...in' walks the keys of an object, found '" + type->name() + "'");
  } else {
    ok = !type->isError();
  }
  llvm::SMLoc loc = object->loc();
  auto *callee = ctx_.make<MemberExpr>(loc, ctx_.make<NameExpr>(loc, "Object"), "keys");
  auto *keys = ctx_.make<CallExpr>(loc, callee, std::vector<Expr *>{object});
  keys->builtin = Builtin::ObjectKeys;
  keys->type = ok ? Type::array(Type::string()) : Type::error();
  keys->forIn = true;
  stmt->iterable = keys;
}

Expr *Sema::checkField(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isClass())
    return checkClassMember(member, nullptr);
  const Field *field = type->field(member->name);
  if (!field) {
    diags_.error(member->loc(), noField(type, member->name));
    member->type = Type::error();
    return member;
  }
  member->type = field->type;
  if (Expr *narrowed = narrowedField(member); narrowed != member)
    return narrowed;
  if (field->type->isOptional())
    if (std::optional<FieldPath> path = fieldPath(member); path && isFieldDefined(*path))
      return convert(member, field->type->inner());
  return member;
}

// `o.f(x)` calls the function in the field f, or a method of an interface.
Expr *Sema::checkFieldCall(CallExpr *call, MemberExpr *member) {
  if (member->object->type->overload(member->name))
    return checkOverloadCall(call, member);
  call->callee = checkField(member);
  const Type *type = call->callee->type;
  if (type->isFunction())
    return checkValueCall(call, type);
  if (type->isOptional() && type->inner()->isFunction())
    diags_.error(member->loc(), "the function may be undefined; check it first, or call it with '?.()', "
                                "which gives undefined then");
  else if (!type->isError())
    diags_.error(member->loc(), "field '" + member->name + "' is not a function; it has type '" +
                                    type->name() + "'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  call->type = Type::error();
  return call;
}

const Type *Sema::checkObjectFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isOptional()) {
    diags_.error(member->object->loc(), "the value may be undefined; check it before assigning to '" +
                                            member->name + "'");
    return Type::error();
  }
  if (type->isTuple() && member->name == "length") {
    diags_.error(member->loc(), "a tuple has a fixed length, which cannot be changed");
    return Type::error();
  }
  const Field *field = type->field(member->name);
  if (!field) {
    if (type->overload(member->name))
      diags_.error(member->loc(), "'" + member->name + "' is a method of '" + type->name() +
                                      "' with several signatures and cannot be replaced");
    else
      diags_.error(member->loc(), noField(type, member->name));
    return Type::error();
  }
  if (field->readonly)
    diags_.error(member->loc(), "cannot assign to '" + member->name + "' because it is a read-only field of '" +
                                    type->name() + "'");
  member->type = field->type;
  return field->type;
}

std::string Sema::objectMismatch(const Type *from, const Type *to, bool conflict) const {
  for (const Field &f : from->fields())
    if (!to->field(f.name))
      return "; an object must have exactly the fields of its type, and this one has the extra field '" +
             f.name + "'";
  for (const Field &t : to->fields())
    if (!from->field(t.name))
      return "; it has no field '" + t.name + "'";
  for (const Field &f : from->fields()) {
    const Field *t = to->field(f.name);
    if (t->type != f.type) {
      std::string message = ("; field '" + f.name + "' is '" + f.type->name() + "' here and '" +
                             t->type->name() + "' in '" + to->name() + "'")
                                .str();
      // `const p = { x: 1 }` made x an int where a double is wanted.
      if (!conflict && f.type->nonOptional()->isNumeric() && t->type->nonOptional()->isNumeric())
        message += ": give the object its type where it is created, e.g. 'const p: " + to->name().str() +
                   " = { ... }'";
      return message;
    }
    if (f.readonly && !t->readonly)
      return "; field '" + f.name + "' is read-only here, and '" + to->name().str() + "' would let it be changed";
  }
  return "";
}

//===----------------------------------------------------------------------===//
// Fields that hold a value
//===----------------------------------------------------------------------===//

std::optional<Sema::FieldPath> Sema::fieldPath(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(unnarrowed(expr));
  if (!member || member->optional || !member->object->type ||
      !(member->object->type->isObject() || member->object->type->isUnion()))
    return std::nullopt;
  const Expr *object = unnarrowed(member->object);
  FieldPath path;
  if (auto *name = dyn_cast<NameExpr>(object)) {
    if (!name->var)
      return std::nullopt;
    path.var = name->var;
  } else if (std::optional<FieldPath> outer = fieldPath(object)) {
    path = std::move(*outer);
  } else {
    return std::nullopt;
  }
  path.names.push_back(member->name);
  return path;
}

bool Sema::isFieldDefined(const FieldPath &path) const {
  return llvm::is_contained(flow_.definedFields, path);
}

void Sema::defineField(Flow &flow, const FieldPath &path) {
  if (!llvm::is_contained(flow.definedFields, path))
    flow.definedFields.push_back(path);
}

void Sema::fieldsAssigned(llvm::StringRef name) {
  // Any object with a field of this name may be the one assigned to.
  llvm::erase_if(flow_.definedFields,
                 [&](const FieldPath &path) { return llvm::is_contained(path.names, name); });
  llvm::erase_if(flow_.narrowedFields, [&](const auto &entry) {
    return llvm::is_contained(entry.first.names, name);
  });
}

void Sema::forgetFieldsOf(Flow &flow, const VarSymbol *var) {
  llvm::erase_if(flow.definedFields, [&](const FieldPath &path) { return path.var == var; });
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first.var == var; });
}

Expr *Sema::narrowedField(MemberExpr *member) {
  // A stream after its setEncoding() (sema_stream.cpp).
  if (!streamViews_.empty())
    if (std::optional<FieldPath> path = fieldPath(member))
      if (const Type *view = streamViewOf(*path, member->loc()))
        return convert(member, view);
  if (flow_.narrowedFields.empty())
    return member;
  std::optional<FieldPath> path = fieldPath(member);
  if (!path)
    return member;
  for (const auto &[narrowed, type] : flow_.narrowedFields)
    if (narrowed == *path)
      return convert(member, type);
  return member;
}

} // namespace tz
