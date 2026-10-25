#include "compiler/program/types/types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <tuple>

namespace tz {

unsigned Type::count_ = 0;

bool Type::isManaged() const {
  switch (kind_) {
  case Kind::Decimal: // its long form
  case Kind::String:
  case Kind::Array:
  case Kind::Function: // its environment
  case Kind::Object:
  case Kind::Iterator:
  case Kind::Json:
  case Kind::Dict:
  case Kind::Promise:
  case Kind::Unknown:
    return true;
  case Kind::Optional:
    return inner_->isManaged();
  case Kind::Result:
    return inner_->isManaged() || error_->isManaged();
  case Kind::Union:
    return llvm::any_of(params_, [](const Type *m) { return m->isManaged(); });
  default: // a literal: a string one is its atom, which is not counted
    return false;
  }
}

bool Type::hasZeroValue() const {
  return isNumeric() || isDecimal() || isBool() || isString() || isOptional();
}

unsigned Type::bitWidth() const {
  switch (kind_) {
  case Kind::Int: return 32;
  case Kind::Long:
  case Kind::Double: return 64;
  default: llvm_unreachable("bitWidth() of a non-numeric type");
  }
}

const Type *Type::error() { static const Type t(Kind::Error, "<error>"); return &t; }
const Type *Type::voidTy() { static const Type t(Kind::Void, "void"); return &t; }
const Type *Type::undefinedTy() { static const Type t(Kind::Undefined, "undefined"); return &t; }
const Type *Type::boolTy() { static const Type t(Kind::Bool, "boolean"); return &t; }
const Type *Type::intTy() { static const Type t(Kind::Int, "int"); return &t; }
const Type *Type::longTy() { static const Type t(Kind::Long, "long"); return &t; }
const Type *Type::doubleTy() { static const Type t(Kind::Double, "double"); return &t; }
const Type *Type::decimalTy() { static const Type t(Kind::Decimal, "decimal"); return &t; }
const Type *Type::string() { static const Type t(Kind::String, "string"); return &t; }
const Type *Type::never() { static const Type t(Kind::Never, "never"); return &t; }
const Type *Type::json() { static const Type t(Kind::Json, "Json"); return &t; }
const Type *Type::unknown() { static const Type t(Kind::Unknown, "unknown"); return &t; }

namespace {

// Composite types, created on first use and kept for the whole compilation.
struct Interned {
  llvm::DenseMap<const Type *, std::unique_ptr<Type>> optionals, arrays;
  using FunctionKey = std::tuple<std::vector<const Type *>, size_t, bool, const Type *, bool>;
  std::map<FunctionKey, std::unique_ptr<Type>> functions;
  // Object types by their fields, sorted by name, and the signatures of
  // their methods with several (marked 2); the recursive ones apart.
  using ObjectKey = std::vector<std::tuple<std::string, const Type *, int>>;
  std::map<ObjectKey, const Type *> objectsByKey;
  std::vector<std::unique_ptr<Type>> objects;
  std::map<std::pair<std::vector<const Type *>, bool>, const Type *> tuples;
  std::map<std::tuple<std::vector<const Type *>, size_t, const Type *>, const Type *> lists;
  std::map<std::tuple<int, std::vector<const Type *>, int, int>, const Type *> ops;
  std::map<std::pair<const Type *, const Type *>, std::unique_ptr<Type>> results;
  std::map<std::tuple<const Type *, const Type *, const Type *>, std::unique_ptr<Type>> iterators;
  std::map<std::tuple<const Type *, const Type *, const Type *>, const Type *> asyncIterators;
  llvm::DenseMap<const Type *, std::unique_ptr<Type>> promises;
  using MatchKey = std::tuple<int, std::vector<bool>, const Type *, bool>;
  std::map<MatchKey, const Type *> matches;
  std::map<std::tuple<int, const Type *, const Type *>, std::unique_ptr<Type>> dicts;
  std::map<std::u16string, std::unique_ptr<Type>> strings;
  std::map<std::pair<std::vector<std::u16string>, std::vector<const Type *>>, std::unique_ptr<Type>>
      patterns;
  std::vector<const Type *> atoms{nullptr}; // by atom, from 1
  std::map<std::pair<double, const Type *>, std::unique_ptr<Type>> numbers;
  // Unions by their members, sorted by id.
  std::map<std::vector<const Type *>, std::unique_ptr<Type>> unions;
  std::map<const ClassDecl *, std::unique_ptr<Type>> classRefs;
  llvm::DenseMap<const Type *, std::unique_ptr<Type>> constructors;
};

Interned &interned() {
  static Interned instance;
  return instance;
}

} // namespace

const Type *Type::optional(const Type *type) {
  // unknown holds undefined already.
  if (type->isError() || type->isUndefined() || type->isOptional() || type->isUnknown())
    return type;
  std::unique_ptr<Type> &slot = interned().optionals[type];
  if (!slot) {
    std::string name = type->isFunction() ? "(" + type->name().str() + ")" : type->name().str();
    slot.reset(new Type(Kind::Optional, name + " | undefined", type));
  }
  return slot.get();
}

const Type *Type::array(const Type *element) {
  if (element->isError())
    return element;
  std::unique_ptr<Type> &slot = interned().arrays[element];
  if (!slot) {
    std::string name = element->isOptional() || element->isFunction() || element->isUnion()
                           ? "(" + element->name().str() + ")[]"
                           : element->name().str() + "[]";
    slot.reset(new Type(Kind::Array, std::move(name), element));
  }
  return slot.get();
}

// `(int, string?) => double`: a parameter that may be left out shows its value
// type with `?`.
const Type *Type::function(llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                           const Type *ret, bool hasThis) {
  for (const Type *t : params)
    if (t->isError())
      return t;
  if (ret->isError())
    return ret;
  // `(...args: [int, string?]) => R` is `(a: int, b?: string) => R`: a list
  // of the types of parameters takes their place.
  std::vector<const Type *> flat;
  if (flattenArguments(params, minArgs, hasRest, flat))
    return function(flat, minArgs, hasRest, ret, hasThis);
  Interned::FunctionKey key{std::vector<const Type *>(params.begin(), params.end()), minArgs, hasRest, ret,
                            hasThis};
  std::unique_ptr<Type> &slot = interned().functions[key];
  if (!slot) {
    std::string name = "(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0)
        name += ", ";
      if (hasThis && i == 0)
        name += "this: " + params[i]->name().str();
      else if (hasRest && i + 1 == params.size())
        name += "..." + params[i]->name().str();
      else if (i >= minArgs && params[i]->isOptional())
        name += params[i]->inner()->name().str() + "?";
      else
        name += params[i]->name().str();
    }
    name += ") => " + ret->name().str();
    slot.reset(new Type(Kind::Function, std::move(name), ret));
    slot->params_.assign(params.begin(), params.end());
    slot->minArgs_ = minArgs;
    slot->hasRest_ = hasRest;
    slot->hasThis_ = hasThis;
  }
  return slot.get();
}

const Type *spreadTuple(llvm::ArrayRef<const Type *> parts, const std::vector<bool> &spread, std::string &why) {
  std::vector<const Type *> elements;
  size_t min = 0;
  const Type *rest = nullptr;
  bool generic = false;
  for (size_t i = 0; i < parts.size(); ++i) {
    const Type *part = parts[i];
    if (part->isError())
      return part;
    if (rest) {
      why = "nothing can follow the array spread into a list: it takes the arguments to the end";
      return nullptr;
    }
    if (i >= spread.size() || !spread[i]) {
      elements.push_back(part);
      min = elements.size(); // the elements before it are needed then
      continue;
    }
    // A list of types in generic code stays as it is, spread there.
    if (part->isListParam()) {
      elements.push_back(part);
      min = elements.size();
      generic = true;
      continue;
    }
    if (part->isArray()) {
      rest = part;
      continue;
    }
    if (!part->isTuple()) {
      why = "only a list of types ('A extends unknown[]'), a tuple or an array can be spread into "
            "a tuple type, not '" + part->name().str() + "'";
      return nullptr;
    }
    bool needed = min == elements.size();
    for (const Field &f : part->fields())
      elements.push_back(f.type);
    generic |= llvm::any_of(part->fields(), [](const Field &f) { return f.type->isListParam(); });
    min = needed ? elements.size() - part->fields().size() + part->tupleMin() : min;
    rest = part->tupleRest();
  }
  if (generic) {
    if (rest) {
      why = "a list of types and an array cannot be spread into one tuple type";
      return nullptr;
    }
    // `[...A]` is A.
    if (elements.size() == 1 && elements[0]->isListParam())
      return elements[0];
    return Type::tuple(elements);
  }
  return Type::argumentList(elements, min, rest);
}

bool flattenArguments(llvm::ArrayRef<const Type *> params, size_t &minArgs, bool &hasRest,
                      std::vector<const Type *> &flat) {
  if (!hasRest || params.empty())
    return false;
  const Type *list = params.back();
  if (!list->isTuple() || llvm::any_of(list->fields(), [](const Field &f) { return f.type->isListParam(); }))
    return false;
  size_t fixed = params.size() - 1;
  flat.assign(params.begin(), params.end() - 1);
  for (const Field &f : list->fields())
    flat.push_back(f.type);
  // The elements a call must give are needed if every parameter before them
  // is; and after them, the rest parameter of the list, if it has one.
  if (minArgs >= fixed)
    minArgs = fixed + list->tupleMin();
  hasRest = list->tupleRest() != nullptr;
  if (hasRest)
    flat.push_back(list->tupleRest());
  return true;
}

std::string objectTypeSpelling(llvm::ArrayRef<Field> fields) {
  if (fields.empty())
    return "{}";
  std::string text = "{ ";
  for (size_t i = 0; i < fields.size(); ++i) {
    const Field &f = fields[i];
    if (i > 0)
      text += "; ";
    if (f.readonly)
      text += "readonly ";
    text += f.name;
    if (f.type->isOptional())
      text += "?: " + f.type->inner()->name().str();
    else
      text += ": " + f.type->name().str();
  }
  return text + " }";
}

const Field *Type::field(llvm::StringRef name) const {
  for (const Field &f : fields_)
    if (f.name == name)
      return &f;
  return nullptr;
}

static Interned::ObjectKey objectKey(llvm::ArrayRef<Field> fields,
                                     llvm::ArrayRef<Overload> overloads) {
  Interned::ObjectKey key;
  for (const Field &f : fields)
    key.emplace_back(f.name, f.type, f.readonly ? 1 : 0);
  for (const Overload &o : overloads)
    for (const Type *signature : o.signatures)
      key.emplace_back(o.name, signature, 2);
  llvm::sort(key);
  return key;
}

const Overload *Type::overload(llvm::StringRef name) const {
  for (const Overload &o : overloads_)
    if (o.name == name)
      return &o;
  return nullptr;
}

const Type *Type::object(std::vector<Field> fields, llvm::StringRef name,
                         std::vector<Overload> overloads) {
  for (const Field &f : fields)
    if (f.type->isError())
      return f.type;
  const Type *&slot = interned().objectsByKey[objectKey(fields, overloads)];
  if (!slot) {
    std::string spelling = name.empty() ? objectTypeSpelling(fields) : name.str();
    auto &type = interned().objects.emplace_back(new Type(Kind::Object, std::move(spelling)));
    type->fields_ = std::move(fields);
    type->overloads_ = std::move(overloads);
    slot = type.get();
  }
  return slot;
}

const Type *Type::tuple(llvm::ArrayRef<const Type *> elements, bool readonly) {
  for (const Type *t : elements)
    if (t->isError())
      return t;
  const Type *&slot =
      interned().tuples[{std::vector<const Type *>(elements.begin(), elements.end()), readonly}];
  if (!slot) {
    std::string name = readonly ? "readonly [" : "[";
    std::vector<Field> fields;
    for (size_t i = 0; i < elements.size(); ++i) {
      if (i > 0)
        name += ", ";
      name += (elements[i]->isListParam() ? "..." : "") + elements[i]->name().str();
      fields.push_back({std::to_string(i), elements[i], readonly});
    }
    auto &type = interned().objects.emplace_back(new Type(Kind::Object, name + "]"));
    type->fields_ = std::move(fields);
    type->tuple_ = true;
    type->tupleMin_ = elements.size();
    slot = type.get();
  }
  return slot;
}

const Type *Type::argumentList(llvm::ArrayRef<const Type *> elements, size_t min, const Type *rest) {
  if (min >= elements.size() && !rest)
    return tuple(elements);
  for (const Type *t : elements)
    if (t->isError())
      return t;
  if (rest && rest->isError())
    return rest;
  const Type *&slot =
      interned().lists[{std::vector<const Type *>(elements.begin(), elements.end()), min, rest}];
  if (!slot) {
    std::string name = "[";
    std::vector<Field> fields;
    for (size_t i = 0; i < elements.size(); ++i) {
      if (i > 0)
        name += ", ";
      // `string?` for an element a call may leave out.
      name += i >= min && elements[i]->isOptional() ? elements[i]->inner()->name().str() + "?"
                                                    : elements[i]->name().str();
      fields.push_back({std::to_string(i), elements[i], false});
    }
    if (rest)
      name += (elements.empty() ? "..." : ", ...") + rest->name().str();
    auto &type = interned().objects.emplace_back(new Type(Kind::Object, name + "]"));
    type->fields_ = std::move(fields);
    type->tuple_ = true;
    type->tupleMin_ = min;
    type->tupleRest_ = rest;
    slot = type.get();
  }
  return slot;
}

const Type *Type::result(const Type *ok, const Type *err) {
  if (ok->isError())
    return ok;
  if (err->isError())
    return err;
  if (ok->isUndefined())
    ok = voidTy();
  if (err->isUndefined())
    err = voidTy();
  std::unique_ptr<Type> &slot = interned().results[{ok, err}];
  if (!slot) {
    std::string name = "Result<" + ok->name().str() + ", " + err->name().str() + ">";
    slot.reset(new Type(Kind::Result, std::move(name), ok));
    slot->error_ = err;
  }
  return slot.get();
}

// `Generator<int>`, `Generator<int, string>`, `Generator<string, void,
// string>`: the arguments that are void at the end are left out.
const Type *Type::iterator(const Type *yielded, const Type *returned, const Type *sent) {
  for (const Type *t : {yielded, returned, sent})
    if (t->isError())
      return t;
  std::unique_ptr<Type> &slot = interned().iterators[{yielded, returned, sent}];
  if (!slot) {
    std::string name = "Generator<" + yielded->name().str();
    if (!returned->isVoid() || !sent->isVoid())
      name += ", " + returned->name().str();
    if (!sent->isVoid())
      name += ", " + sent->name().str();
    slot.reset(new Type(Kind::Iterator, name + ">", yielded));
    slot->params_ = {returned, sent};
  }
  return slot.get();
}

const Type *Type::dict(DictKind kind, const Type *key, const Type *value) {
  if (key->isError())
    return key;
  if (value->isError())
    return value;
  std::unique_ptr<Type> &slot = interned().dicts[{int(kind), key, value}];
  if (!slot) {
    std::string name = kind == DictKind::Record ? "Record<"
                       : kind == DictKind::Map  ? "Map<"
                                                : "Set<";
    name += key->name().str();
    if (kind != DictKind::Set)
      name += ", " + value->name().str();
    slot.reset(new Type(Kind::Dict, name + ">", value));
    slot->params_ = {key};
    slot->dictKind_ = kind;
  }
  return slot.get();
}

// `"circle"`, with the characters a string literal escapes.
static std::string quotedText(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);
  std::string quoted = "\"";
  for (char c : utf8) {
    if (c == '\n')
      quoted += "\\n";
    else if (c == '\t')
      quoted += "\\t";
    else if (c == '"' || c == '\\')
      quoted += std::string("\\") + c;
    else
      quoted += c;
  }
  return quoted + "\"";
}

// The shortest text that reads back as `value`, as JavaScript prints most
// numbers.
static std::string numberText(double value) {
  if (value == std::trunc(value) && std::fabs(value) < 1e21) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.0f", value);
    return buffer;
  }
  for (int precision = 1; precision <= 17; ++precision) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
    if (std::strtod(buffer, nullptr) == value)
      return buffer;
  }
  return std::to_string(value);
}

static std::string utf8Text(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);
  return utf8;
}

const Type *Type::stringPattern(std::vector<std::u16string> texts,
                                std::vector<const Type *> holes) {
  // `${string}` is any string.
  if (holes.size() == 1 && holes[0]->isString() && texts[0].empty() && texts[1].empty())
    return holes[0];
  std::unique_ptr<Type> &slot = interned().patterns[{texts, holes}];
  if (!slot) {
    std::string name = "`";
    for (size_t i = 0; i < texts.size(); ++i) {
      name += utf8Text(texts[i]);
      if (i < holes.size())
        name += "${" + (holes[i]->isFloat() ? std::string("number") : holes[i]->name().str()) + "}";
    }
    slot.reset(new Type(Kind::String, name + "`"));
    slot->pattern_ = true;
    slot->texts_ = std::move(texts);
    slot->params_ = std::move(holes);
  }
  return slot.get();
}

const Type *Type::stringLiteral(const std::u16string &text) {
  std::unique_ptr<Type> &slot = interned().strings[text];
  if (!slot) {
    slot.reset(new Type(Kind::Literal, quotedText(text), string()));
    slot->text_ = text;
    slot->atom_ = unsigned(interned().atoms.size());
    interned().atoms.push_back(slot.get());
  }
  return slot.get();
}

const Type *Type::numberLiteral(double value, const Type *base) {
  std::unique_ptr<Type> &slot = interned().numbers[{value, base}];
  if (!slot) {
    slot.reset(new Type(Kind::Literal, numberText(value), base));
    slot->number_ = value;
  }
  return slot.get();
}

const Type *Type::boolLiteral(bool value) {
  static const Type *literals[2] = {nullptr, nullptr};
  const Type *&slot = literals[value ? 1 : 0];
  if (!slot) {
    auto *literal = new Type(Kind::Literal, value ? "true" : "false", boolTy());
    Type *type = interned().objects.emplace_back(literal).get();
    type->number_ = value ? 1 : 0;
    slot = type;
  }
  return slot;
}

llvm::ArrayRef<const Type *> Type::stringLiterals() {
  return llvm::ArrayRef<const Type *>(interned().atoms).drop_front();
}

UnionSlot unionSlot(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::String: return UnionSlot::String;
  case Type::Kind::Int: return UnionSlot::Int;
  case Type::Kind::Long: return UnionSlot::Long;
  case Type::Kind::Double: return UnionSlot::Double;
  case Type::Kind::Bool: return UnionSlot::Bool;
  case Type::Kind::Decimal: return UnionSlot::Decimal;
  case Type::Kind::Object: return UnionSlot::Object;
  case Type::Kind::Dict: return UnionSlot::Object; // its layout tells it apart
  case Type::Kind::Promise: return UnionSlot::Object; // so does a Promise's
  case Type::Kind::ClassRef: return UnionSlot::Object; // and a class's
  case Type::Kind::Array: return UnionSlot::Array;
  case Type::Kind::Function: return UnionSlot::Function;
  case Type::Kind::Iterator: return UnionSlot::Iterator;
  case Type::Kind::Param: return UnionSlot::Param;
  case Type::Kind::Literal: {
    const Type *base = type->literalBase();
    return base->isString() ? UnionSlot::Atom : unionSlot(base);
  }
  case Type::Kind::Union:
    return type->isTagged() ? UnionSlot::None : type->slots().front();
  default:
    return UnionSlot::Other;
  }
}

const Type *Type::slotPart(UnionSlot slot) const {
  if (!isUnion())
    return unionSlot(this) == slot ? this : nullptr;
  std::vector<const Type *> part;
  for (const Type *m : params_)
    if (unionSlot(m) == slot)
      part.push_back(m);
  if (part.empty())
    return nullptr;
  return part.size() == 1 ? part.front() : unionOf(part);
}

const Type *Type::unionOf(llvm::ArrayRef<const Type *> types) {
  std::vector<const Type *> members;
  bool undefined = false;
  std::function<bool(const Type *)> add = [&](const Type *t) {
    switch (t->kind()) {
    case Kind::Error:
      return false;
    case Kind::Never:
      return true;
    case Kind::Undefined:
      undefined = true;
      return true;
    case Kind::Optional:
      undefined = true;
      return add(t->inner());
    case Kind::Union:
      for (const Type *m : t->members())
        add(m);
      return true;
    default:
      if (!llvm::is_contained(members, t))
        members.push_back(t);
      return true;
    }
  };
  for (const Type *t : types)
    if (!add(t))
      return t;
  auto has = [&](const Type *t) { return llvm::is_contained(members, t); };
  // An unknown holds any value already, as in TypeScript: `unknown | int` is
  // unknown.
  if (has(unknown()))
    return unknown();
  // `true | false` is boolean.
  if (has(boolLiteral(true)) && has(boolLiteral(false))) {
    llvm::erase(members, boolLiteral(true));
    llvm::erase(members, boolLiteral(false));
    if (!has(boolTy()))
      members.push_back(boolTy());
  }
  // A literal is a value of its base type.
  llvm::erase_if(members, [&](const Type *m) {
    if (!m->isLiteral())
      return false;
    const Type *base = m->literalBase();
    if (base->isInt())
      return has(intTy()) || has(longTy()) || has(doubleTy());
    return has(base);
  });
  // A string of a pattern is a string, and a string literal that matches a
  // pattern is a string of it.
  if (has(string()))
    llvm::erase_if(members, [](const Type *m) { return m->isPattern(); });
  llvm::erase_if(members, [&](const Type *m) {
    return m->isStringLiteral() && llvm::any_of(members, [&](const Type *p) {
             return p->isPattern() && matchesPattern(m->literalText(), p);
           });
  });
  // Whole numbers next to fractions are doubles too: `0 | 0.5`.
  bool fractions = llvm::any_of(members, [](const Type *m) {
    return m->isLiteral() && m->literalBase()->isFloat();
  });
  if (fractions)
    for (const Type *&m : members)
      if (m->isLiteral() && m->literalBase()->isInt())
        m = numberLiteral(m->literalNumber(), doubleTy());
  // Some object, in generic code, holds any other; an object that is
  // declared to be another member is that member.
  if (llvm::any_of(members, [](const Type *m) { return m->isAnyObject(); }))
    llvm::erase_if(members, [](const Type *m) { return m->isObject() && !m->isAnyObject(); });
  std::vector<const Type *> reduced;
  for (const Type *m : members)
    if (!m->isObject() || llvm::none_of(members, [&](const Type *o) {
          return o != m && o->isObject() && m->isSubtypeOf(o);
        }))
      if (!llvm::is_contained(reduced, m))
        reduced.push_back(m);
  members = std::move(reduced);

  const Type *result;
  if (members.empty())
    return undefined ? undefinedTy() : never();
  if (members.size() == 1) {
    result = members.front();
  } else {
    std::vector<const Type *> key = members;
    llvm::sort(key, [](const Type *a, const Type *b) { return a->id() < b->id(); });
    std::unique_ptr<Type> &slot = interned().unions[key];
    if (!slot) {
      std::string name;
      for (const Type *m : members)
        name += (name.empty() ? "" : " | ") +
                (m->isFunction() ? "(" + m->name().str() + ")" : m->name().str());
      slot.reset(new Type(Kind::Union, std::move(name)));
      slot->params_ = members;
      for (const Type *m : members) {
        UnionSlot s = unionSlot(m);
        if (!llvm::is_contained(slot->slots_, s))
          slot->slots_.push_back(s);
      }
      llvm::sort(slot->slots_);
    }
    result = slot.get();
  }
  return undefined ? optional(result) : result;
}

std::string unionProblem(const Type *type) {
  type = type->nonOptional();
  if (!type->isUnion())
    return "";
  const Type *number = nullptr, *array = nullptr, *function = nullptr, *iterator = nullptr,
             *promise = nullptr, *intl = nullptr, *crypto = nullptr;
  for (const Type *m : type->members()) {
    if (m->isVoid())
      return "'void' cannot be part of a union";
    if (m->isJson())
      return "a Json already holds strings, numbers, booleans, arrays and objects, which code "
             "tells apart by checking it; use 'Json'";
    if (m->isResult())
      return "a Result cannot be part of a union: what it holds is told by its halves; make a "
             "half the union instead, e.g. 'Result<int, A | B>'";
    // Two members of slots that one check cannot tell apart.
    auto clash = [&](const Type *&seen, const char *why, const std::string &fix) -> std::string {
      if (!seen) {
        seen = m;
        return "";
      }
      return "'" + seen->name().str() + "' and '" + m->name().str() +
             "' cannot be told apart when the program runs: " + why + "; " + fix;
    };
    std::string problem;
    UnionSlot slot = unionSlot(m);
    if (slot == UnionSlot::Int || slot == UnionSlot::Long || slot == UnionSlot::Double) {
      if (!number || unionSlot(number) != slot)
        problem = clash(number, "'typeof' gives \"number\" for both",
                        "use one number type, e.g. 'double'");
    } else if (slot == UnionSlot::Array) {
      problem = clash(array, "both are arrays",
                      "use one array type, e.g. '(" +
                          (array ? array->inner()->name().str() : std::string()) + " | " +
                          m->inner()->name().str() + ")[]'");
    } else if (slot == UnionSlot::Function) {
      problem = clash(function, "'typeof' gives \"function\" for both", "use one function type");
    } else if (slot == UnionSlot::Iterator) {
      problem = clash(iterator, "both are iterators", "use one iterator type");
    } else if (m->isIntl()) {
      // The runtime makes them, and they carry no number of their own.
      problem = clash(intl, "both are objects of Intl, which carry nothing a check could tell "
                            "apart by", "keep each in a variable of its own type");
    } else if (m->isCrypto()) {
      // So are those of node:crypto.
      problem = clash(crypto, "both are objects of node:crypto, which carry nothing a check "
                              "could tell apart by", "keep each in a variable of its own type");
    } else if (m->isPromise()) {
      problem = clash(promise, "both are promises",
                      "use one Promise type, e.g. 'Promise<" +
                          (promise ? promise->promised()->name().str() : std::string()) + " | " +
                          m->promised()->name().str() + ">'");
    }
    if (!problem.empty())
      return problem;
  }
  return "";
}

const Type *unionWithout(const Type *type, const Type *without) {
  llvm::ArrayRef<const Type *> removed =
      without->isUnion() ? without->members() : llvm::ArrayRef<const Type *>(without);
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  std::vector<const Type *> rest;
  for (const Type *m : members) {
    // boolean without true is false.
    if (m->isBool()) {
      bool noTrue = llvm::is_contained(removed, Type::boolLiteral(true));
      bool noFalse = llvm::is_contained(removed, Type::boolLiteral(false));
      if (noTrue != noFalse) {
        rest.push_back(Type::boolLiteral(noFalse));
        continue;
      }
    }
    if (llvm::none_of(removed, [&](const Type *r) {
          return m == r || (m->isObject() && r->isObject() && m->isSubtypeOf(r));
        }))
      rest.push_back(m);
  }
  if (rest.empty())
    return nullptr;
  return Type::unionOf(rest);
}

const Type *Type::promise(const Type *value) {
  if (value->isError())
    return value;
  if (value->isUndefined())
    value = voidTy();
  std::unique_ptr<Type> &slot = interned().promises[value];
  if (!slot)
    slot.reset(new Type(Kind::Promise, "Promise<" + value->name().str() + ">", value));
  return slot.get();
}

Type *Type::declareObject(llvm::StringRef name) {
  auto &type = interned().objects.emplace_back(new Type(Kind::Object, name.str()));
  type->incomplete_ = true;
  return type.get();
}

Type *Type::classType(llvm::StringRef name, const ClassDecl *decl) {
  auto &type = interned().objects.emplace_back(new Type(Kind::Object, name.str()));
  type->incomplete_ = true;
  type->cls_ = decl;
  return type.get();
}

Type *Type::declareUnion(llvm::StringRef name) {
  auto &type = interned().objects.emplace_back(new Type(Kind::Union, name.str()));
  type->incomplete_ = true;
  return type.get();
}

void Type::defineMembers(const Type *of) {
  params_.assign(of->params_.begin(), of->params_.end());
  slots_ = of->slots_;
  incomplete_ = false;
}

void Type::defineFields(std::vector<Field> fields, std::vector<Overload> overloads) {
  fields_ = std::move(fields);
  overloads_ = std::move(overloads);
  incomplete_ = false;
}

void Type::addBase(const Type *base) {
  if (base == this || llvm::is_contained(bases_, base))
    return;
  bases_.push_back(base);
  const_cast<Type *>(base)->isBase_ = true;
}

void Type::swapClasses(Type *a, Type *b) {
  std::swap(a->cls_, b->cls_);
  std::swap(a->fields_, b->fields_);
  std::swap(a->overloads_, b->overloads_);
  std::swap(a->incomplete_, b->incomplete_);
  std::swap(a->bases_, b->bases_);
  std::swap(a->isBase_, b->isBase_);
  for (Type *t : {a, b})
    for (const Type *&base : t->bases_)
      base = base == a ? b : base == b ? a : base;
}

bool Type::isSubtypeOf(const Type *base) const {
  if (this == base)
    return true;
  for (const Type *b : bases_)
    if (b->isSubtypeOf(base))
      return true;
  return false;
}

Type *Type::param(llvm::StringRef name) {
  return interned().objects.emplace_back(new Type(Kind::Param, name.str())).get();
}

const Type *Type::typeOp(TypeOp op, llvm::ArrayRef<const Type *> operands) {
  const Type *&slot =
      interned().ops[{int(op), std::vector<const Type *>(operands.begin(), operands.end()), 0, 0}];
  if (!slot) {
    std::string name;
    auto of = [&](size_t i) { return i < operands.size() && operands[i] ? operands[i]->name().str() : ""; };
    switch (op) {
    case TypeOp::KeyOf: name = "keyof " + of(0); break;
    // `A[number]` of any number: the int that indices are.
    case TypeOp::Index: name = of(0) + "[" + (operands[1]->isInt() ? "number" : of(1)) + "]"; break;
    case TypeOp::Exclude: name = "Exclude<" + of(0) + ", " + of(1) + ">"; break;
    case TypeOp::Extract: name = "Extract<" + of(0) + ", " + of(1) + ">"; break;
    case TypeOp::ReturnType: name = "ReturnType<" + of(0) + ">"; break;
    case TypeOp::Parameters: name = "Parameters<" + of(0) + ">"; break;
    case TypeOp::NonNullable: name = "NonNullable<" + of(0) + ">"; break;
    // Named by the text written, which the tag carries (see
    // Sema::resolveConditional).
    case TypeOp::Conditional: name = of(0); break;
    // The texts and the types between them: `on${K}`.
    case TypeOp::Template:
      name = "`";
      for (size_t i = 0; i < operands.size(); ++i)
        name += i % 2 == 0 ? utf8Text(operands[i]->literalText()) : "${" + of(i) + "}";
      name += "`";
      break;
    case TypeOp::Uppercase: name = "Uppercase<" + of(0) + ">"; break;
    case TypeOp::Lowercase: name = "Lowercase<" + of(0) + ">"; break;
    case TypeOp::Capitalize: name = "Capitalize<" + of(0) + ">"; break;
    case TypeOp::Uncapitalize: name = "Uncapitalize<" + of(0) + ">"; break;
    case TypeOp::Spread:
      name = "[";
      for (size_t i = 0; i + 1 < operands.size(); i += 2)
        name += (i ? ", " : "") + std::string(operands[i]->literalNumber() != 0 ? "..." : "") +
                of(i + 1);
      name += "]";
      break;
    default: name = "?"; break;
    }
    Type *type = param(name);
    type->op_ = op;
    type->operands_.assign(operands.begin(), operands.end());
    slot = type;
  }
  return slot;
}

const Type *Type::mapped(const Type *key, const Type *keys, const Type *value, int optional,
                         int readonly, const Type *source, const Type *rename) {
  const Type *&slot =
      interned().ops[{int(TypeOp::Mapped), {key, keys, value, source, rename}, optional, readonly}];
  if (!slot) {
    std::string name = std::string(readonly > 0   ? "{ readonly ["
                                   : readonly < 0 ? "{ -readonly ["
                                                  : "{ [") +
                       key->name().str() + " in " + keys->name().str() +
                       (rename ? " as " + rename->name().str() : "") + "]" +
                       (optional > 0   ? "?"
                        : optional < 0 ? "-?"
                                       : "") +
                       ": " + value->name().str() + " }";
    Type *type = param(name);
    type->op_ = TypeOp::Mapped;
    type->operands_ = {key, keys, value, source, rename};
    type->mappedOptional_ = optional;
    type->mappedReadonly_ = readonly;
    slot = type;
  }
  return slot;
}

void Type::setConstraint(const Type *constraint, std::vector<const Type *> alternatives) {
  inner_ = constraint;
  params_ = std::move(alternatives);
}

const Type *Type::anyObject() {
  static Type *type = [] {
    Type *t = interned().objects.emplace_back(new Type(Kind::Object, "object")).get();
    t->anyObject_ = true;
    return t;
  }();
  return type;
}

const Type *Type::classRef(const ClassDecl *cls, llvm::StringRef name) {
  std::unique_ptr<Type> &slot = interned().classRefs[cls];
  if (!slot) {
    slot.reset(new Type(Kind::ClassRef, "typeof " + name.str()));
    slot->refClass_ = cls;
  }
  return slot.get();
}

const Type *Type::constructorOf(const Type *instance) {
  if (instance->isError())
    return instance;
  std::unique_ptr<Type> &slot = interned().constructors[instance];
  if (!slot)
    slot.reset(new Type(Kind::ClassRef,
                        instance->isAnyObject() ? std::string("Constructor")
                                                : "Constructor<" + instance->name().str() + ">",
                        instance));
  return slot.get();
}

// Not interned with the other object types: no object type a program
// declares is a regex, even one with just `lastIndex: int`.
const Type *Type::regExp() {
  static Type *type = [] {
    Type *t = interned().objects.emplace_back(new Type(Kind::Object, "RegExp")).get();
    t->fields_.push_back({"lastIndex", intTy(), false});
    t->regExp_ = true;
    return t;
  }();
  return type;
}

// Not interned either: a Date is only itself. Its field has a name no program
// can write, so only the methods of Date reach it.
const Type *Type::date() {
  static Type *type = [] {
    Type *t = interned().objects.emplace_back(new Type(Kind::Object, "Date")).get();
    t->fields_.push_back({"%time", doubleTy(), false});
    t->date_ = true;
    return t;
  }();
  return type;
}

// Not interned with the object types: each WeakRef<T> is only itself, and
// what the runtime keeps in it the program does not see
// (runtime/core/group/group.c).
const Type *Type::weakRef(const Type *target) {
  static std::map<const Type *, Type *> types;
  Type *&type = types[target];
  if (!type) {
    type = interned().objects.emplace_back(new Type(Kind::Object, "WeakRef<" + target->name().str() + ">", target)).get();
    type->weakRef_ = true;
  }
  return type;
}

// What the runtime keeps in one (a formatter of ICU) the program does not
// see: the type has no fields.
const Type *Type::dateFormat() { return intl(IntlKind::DateTimeFormat); }

// So with the other objects of Intl: a type of its own for each kind, which
// no object type a program declares is.
const Type *Type::intl(IntlKind kind) {
  static const char *const names[] = {
      "",
      "Intl.DateTimeFormat",
      "Intl.NumberFormat",
      "Intl.Collator",
      "Intl.PluralRules",
      "Intl.RelativeTimeFormat",
      "Intl.ListFormat",
      "Intl.DisplayNames",
      "Intl.Segmenter",
      "Intl.Segments",
  };
  static Type *types[sizeof names / sizeof names[0]] = {};
  Type *&type = types[size_t(kind)];
  if (!type) {
    type = interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->intl_ = kind;
  }
  return type;
}

// The timers of the runtime: their objects hold what the program does not
// see (TzTimer in runtime/core/tzrt/tzrt.h).
const Type *Type::timeout() {
  static Type *type = [] {
    Type *t = interned().objects.emplace_back(new Type(Kind::Object, "Timeout")).get();
    t->timer_ = 1;
    return t;
  }();
  return type;
}

const Type *Type::immediate() {
  static Type *type = [] {
    Type *t = interned().objects.emplace_back(new Type(Kind::Object, "Immediate")).get();
    t->timer_ = 2;
    return t;
  }();
  return type;
}

// Not interned either: each class of node:crypto is only itself, and the
// runtime makes its objects.
const Type *Type::cryptoType(Crypto kind) {
  static const char *const names[] = {"",        "Hash", "Hmac",   "Cipheriv", "Decipheriv",
                                      "KeyObject", "Sign", "Verify", "ECDH",     "X509Certificate"};
  static Type *types[std::size(names)] = {};
  Type *&type = types[size_t(kind)];
  if (!type) {
    type = interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->crypto_ = kind;
  }
  return type;
}

// Not interned either: no object type a program declares is a Uint8Array.
// They have no fields; their methods reach what the runtime keeps in them.
const Type *Type::bytesType(Bytes kind) {
  static const char *const names[] = {
      "",            "Uint8Array",  "Int8Array",    "Uint8ClampedArray", "Int16Array",
      "Uint16Array", "Int32Array",  "Uint32Array",  "Float16Array",      "Float32Array",
      "Float64Array", "Buffer",     "ArrayBuffer",  "DataView",          "TextEncoder",
      "TextDecoder"};
  static Type *types[std::size(names)] = {};
  Type *&type = types[size_t(kind)];
  if (!type) {
    type = interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->bytes_ = kind;
  }
  return type;
}

const Type *Type::runtimeObject(Runtime kind) {
  static Type *types[size_t(Runtime::AsyncIterator)];
  static const char *const names[] = {"", "AbortSignal", "FileHandle", "Interface", "Interface"};
  Type *&type = types[size_t(kind)];
  if (!type) {
    type = interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->runtime_ = kind;
  }
  return type;
}

const Type *Type::elementType() const {
  switch (bytes_) {
  case Bytes::Uint32:
    return longTy();
  case Bytes::Float16:
  case Bytes::Float32:
  case Bytes::Float64:
    return doubleTy();
  default:
    return intTy();
  }
}

unsigned Type::elementSize() const {
  switch (bytes_) {
  case Bytes::Int16:
  case Bytes::Uint16:
  case Bytes::Float16:
    return 2;
  case Bytes::Int32:
  case Bytes::Uint32:
  case Bytes::Float32:
    return 4;
  case Bytes::Float64:
    return 8;
  default:
    return 1;
  }
}

bool Type::elementRange(int64_t &lo, int64_t &hi) const {
  switch (bytes_) {
  case Bytes::Uint8:
  case Bytes::Uint8Clamped:
  case Bytes::Buffer:
    lo = 0, hi = 255;
    return true;
  case Bytes::Int8:
    lo = -128, hi = 127;
    return true;
  case Bytes::Int16:
    lo = -32768, hi = 32767;
    return true;
  case Bytes::Uint16:
    lo = 0, hi = 65535;
    return true;
  case Bytes::Int32:
    lo = INT32_MIN, hi = INT32_MAX;
    return true;
  case Bytes::Uint32:
    lo = 0, hi = UINT32_MAX;
    return true;
  default:
    return false;
  }
}

const Type *Type::asyncIterator(const Type *yielded, const Type *returned, const Type *sent) {
  for (const Type *t : {yielded, returned, sent})
    if (t->isError())
      return t;
  const Type *&slot = interned().asyncIterators[{yielded, returned, sent}];
  if (!slot) {
    std::string name = "AsyncGenerator<" + yielded->name().str();
    if (!returned->isVoid() || !sent->isVoid())
      name += ", " + returned->name().str();
    if (!sent->isVoid())
      name += ", " + sent->name().str();
    Type *type = interned().objects.emplace_back(new Type(Kind::Object, name + ">", yielded)).get();
    type->params_ = {returned, sent};
    type->runtime_ = Runtime::AsyncIterator;
    slot = type;
  }
  return slot;
}

// Every match has the same fields of the same sizes, so the same layout,
// whatever the regex: a match of a known regex can be seen as one of an
// unknown regex without any change in memory.
const Type *Type::match(int groups, std::vector<bool> mandatory, const Type *named, bool indices) {
  if (groups < 0)
    mandatory.clear();
  else
    mandatory.resize(size_t(groups) + 1, false);
  const Type *&slot = interned().matches[{groups, mandatory, named, indices}];
  if (slot)
    return slot;
  std::string name = "RegExpMatchArray";
  if (groups >= 0) {
    name += " [string";
    for (int k = 1; k <= groups; ++k)
      name += mandatory[size_t(k)] ? ", string" : ", string | undefined";
    name += "]";
    if (named)
      name += " with groups " + named->name().str();
  }
  Type *type = interned().objects.emplace_back(new Type(Kind::Object, name)).get();
  type->fields_.push_back({"%items", array(optional(string())), true});
  type->fields_.push_back({"index", intTy(), false});
  type->fields_.push_back({"input", string(), false});
  type->fields_.push_back({"groups", named ? named : optional(object({})), false});
  if (indices)
    type->fields_.push_back({"indices", array(optional(tuple({intTy(), intTy()}))), false});
  type->match_ = true;
  type->matchGroups_ = groups;
  type->mandatory_ = std::move(mandatory);
  slot = type;
  return slot;
}

int subtypeDistance(const Type *from, const Type *to) {
  if (from == to)
    return 0;
  int best = -1;
  for (const Type *base : from->bases()) {
    int d = subtypeDistance(base, to);
    if (d >= 0 && (best < 0 || d + 1 < best))
      best = d + 1;
  }
  return best;
}

static bool mentionsParam(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Param:
    return true;
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return mentionsParam(type->inner(), seen);
  case Type::Kind::Result:
    return mentionsParam(type->okType(), seen) || mentionsParam(type->errType(), seen);
  case Type::Kind::Function:
    return mentionsParam(type->ret(), seen) ||
           llvm::any_of(type->params(), [&](const Type *p) { return mentionsParam(p, seen); });
  case Type::Kind::Iterator:
    return mentionsParam(type->yielded(), seen) || mentionsParam(type->returned(), seen) ||
           mentionsParam(type->sent(), seen);
  case Type::Kind::Dict:
    return mentionsParam(type->keyType(), seen) || mentionsParam(type->valueType(), seen);
  case Type::Kind::ClassRef: // `Constructor<T>`
    return type->isConstructor() && mentionsParam(type->constructed(), seen);
  case Type::Kind::Union:
    return llvm::any_of(type->members(), [&](const Type *m) { return mentionsParam(m, seen); });
  case Type::Kind::Object:
    // An async iterator, as an iterator: by what it gives, returns and takes.
    if (type->isAsyncIterator())
      return mentionsParam(type->yielded(), seen) || mentionsParam(type->returned(), seen) ||
             mentionsParam(type->sent(), seen);
    // A class made from a generic one is generic code by its type
    // arguments, whatever its fields.
    if (!type->typeArgs().empty())
      return llvm::any_of(type->typeArgs(), [&](const Type *t) { return mentionsParam(t, seen); });
    // A recursive type mentions itself.
    if (!seen.insert(type).second)
      return false;
    if (type->tupleRest() && mentionsParam(type->tupleRest(), seen))
      return true;
    return llvm::any_of(type->fields(), [&](const Field &f) { return mentionsParam(f.type, seen); });
  default:
    return false;
  }
}

bool mentionsParam(const Type *type) {
  llvm::SmallPtrSet<const Type *, 4> seen;
  return mentionsParam(type, seen);
}

const char *typeOfName(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Int:
  case Type::Kind::Long:
  case Type::Kind::Double: return "number";
  case Type::Kind::String: return "string";
  case Type::Kind::Bool: return "boolean";
  case Type::Kind::Array:
  case Type::Kind::Object:
  case Type::Kind::Result:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Decimal: return "object"; // as for a BigNumber.js object
  case Type::Kind::Function: return "function";
  case Type::Kind::Optional: return typeOfName(type->inner());
  case Type::Kind::Param: return ""; // not known until a copy gives it a type
  case Type::Kind::Json: return ""; // known when the program runs
  case Type::Kind::Literal: return typeOfName(type->literalBase());
  case Type::Kind::Union: {
    // Known when the program runs, unless every member gives the same.
    const char *name = typeOfName(type->members().front());
    for (const Type *m : type->members())
      if (llvm::StringRef(typeOfName(m)) != name)
        return "";
    return name;
  }
  case Type::Kind::Unknown: return "";
  case Type::Kind::ClassRef: return "function"; // a class is a function in JavaScript
  default: return "undefined";
  }
}

const Type *Type::fromName(llvm::StringRef name) {
  return llvm::StringSwitch<const Type *>(name)
      .Case("void", voidTy())
      .Case("undefined", undefinedTy())
      .Case("boolean", boolTy())
      .Case("int", intTy())
      .Case("long", longTy())
      .Cases({"double", "number"}, doubleTy())
      .Case("decimal", decimalTy())
      .Case("string", string())
      .Case("Json", json())
      .Case("unknown", unknown())
      .Case("object", anyObject())
      .Default(nullptr);
}

std::string jsonTagField(llvm::ArrayRef<const Type *> objects) {
  if (objects.empty() || !objects.front()->isObject())
    return "";
  for (const Field &f : objects.front()->fields()) {
    std::vector<const Type *> tags;
    for (const Type *m : objects) {
      const Field *g = m->isObject() ? m->field(f.name) : nullptr;
      if (!g || !g->type->isLiteral() || llvm::is_contained(tags, g->type))
        break;
      tags.push_back(g->type);
    }
    if (tags.size() == objects.size())
      return f.name;
  }
  return "";
}

bool splitByTemplate(const std::u16string &text, llvm::ArrayRef<std::u16string> texts,
                     std::vector<std::u16string> &parts) {
  const std::u16string &start = texts.front(), &end = texts.back();
  if (text.size() < start.size() + end.size() || text.compare(0, start.size(), start) != 0 ||
      text.compare(text.size() - end.size(), end.size(), end) != 0)
    return false;
  // As TypeScript's inferFromLiteralPartsToTemplateLiteral does.
  std::u16string rest = text.substr(0, text.size() - end.size());
  size_t pos = start.size();
  parts.clear();
  for (size_t i = 1; i + 1 < texts.size(); ++i) {
    const std::u16string &delimiter = texts[i];
    if (!delimiter.empty()) {
      size_t found = rest.find(delimiter, pos);
      if (found == std::u16string::npos)
        return false;
      parts.push_back(rest.substr(pos, found - pos));
      pos = found + delimiter.size();
    } else if (pos < rest.size()) {
      parts.push_back(rest.substr(pos, 1));
      pos += 1;
    } else {
      return false;
    }
  }
  parts.push_back(rest.substr(pos));
  return true;
}

bool numberString(const std::u16string &text, double &value) {
  // What JavaScript's StringToNumber skips around the number.
  auto space = [](char16_t c) {
    return c == ' ' || (c >= '\t' && c <= '\r') || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
           c == 0x205f || c == 0x3000 || c == 0xfeff;
  };
  size_t begin = 0, end = text.size();
  while (begin < end && space(text[begin]))
    ++begin;
  while (end > begin && space(text[end - 1]))
    --end;
  std::string ascii;
  for (size_t i = begin; i < end; ++i) {
    if (text[i] > 0x7f)
      return false;
    ascii += char(text[i]);
  }
  // TypeScript takes only the empty string as no number.
  if (text.empty())
    return false;
  if (ascii.empty()) {
    value = 0;
    return true;
  }
  if (ascii.size() > 2 && ascii[0] == '0' && std::strchr("xXoObB", ascii[1])) {
    int radix = ascii[1] == 'x' || ascii[1] == 'X'   ? 16
                : ascii[1] == 'o' || ascii[1] == 'O' ? 8
                                                     : 2;
    value = 0;
    for (size_t i = 2; i < ascii.size(); ++i) {
      int digit = std::isdigit(static_cast<unsigned char>(ascii[i])) ? ascii[i] - '0'
                  : std::isxdigit(static_cast<unsigned char>(ascii[i]))
                      ? std::tolower(static_cast<unsigned char>(ascii[i])) - 'a' + 10
                      : 99;
      if (digit >= radix)
        return false;
      value = value * radix + digit;
    }
    return std::isfinite(value);
  }
  // [+-] digits [. digits] [e [+-] digits], with a digit before or after
  // the point.
  size_t i = 0, digits = 0;
  if (ascii[i] == '+' || ascii[i] == '-')
    ++i;
  while (i < ascii.size() && std::isdigit(static_cast<unsigned char>(ascii[i])))
    ++i, ++digits;
  if (i < ascii.size() && ascii[i] == '.') {
    ++i;
    while (i < ascii.size() && std::isdigit(static_cast<unsigned char>(ascii[i])))
      ++i, ++digits;
  }
  if (digits == 0)
    return false; // Infinity too: not a finite number
  if (i < ascii.size() && (ascii[i] == 'e' || ascii[i] == 'E')) {
    ++i;
    if (i < ascii.size() && (ascii[i] == '+' || ascii[i] == '-'))
      ++i;
    size_t exponent = 0;
    while (i < ascii.size() && std::isdigit(static_cast<unsigned char>(ascii[i])))
      ++i, ++exponent;
    if (exponent == 0)
      return false;
  }
  if (i != ascii.size())
    return false;
  value = std::strtod(ascii.c_str(), nullptr);
  return std::isfinite(value);
}

bool holeAccepts(const Type *hole, const std::u16string &part) {
  double value = 0;
  switch (hole->kind()) {
  case Type::Kind::String:
    return !hole->isPattern() || matchesPattern(part, hole);
  case Type::Kind::Double:
    return numberString(part, value);
  // `${int}`: a whole number that fits.
  case Type::Kind::Int:
    return numberString(part, value) && value == std::trunc(value) && value >= -2147483648.0 &&
           value <= 2147483647.0;
  case Type::Kind::Long:
    return numberString(part, value) && value == std::trunc(value) &&
           std::fabs(value) <= 9223372036854775807.0;
  case Type::Kind::Literal:
    return hole->isStringLiteral() ? part == hole->literalText() : utf8Text(part) == hole->name();
  case Type::Kind::Union:
    return llvm::any_of(hole->members(), [&](const Type *m) { return holeAccepts(m, part); });
  default: return false;
  }
}

bool matchesPattern(const std::u16string &text, const Type *pattern) {
  std::vector<std::u16string> parts;
  if (!splitByTemplate(text, pattern->patternTexts(), parts))
    return false;
  for (size_t i = 0; i < parts.size(); ++i)
    if (!holeAccepts(pattern->patternHoles()[i], parts[i]))
      return false;
  return true;
}

} // namespace tz
