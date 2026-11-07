#include "compiler/program/types/type/type.h"
#include "compiler/program/types/argument_lists/argument_lists.h"
#include "compiler/program/types/template_patterns/template_patterns.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/program/types/union_slots/union_slots.h"
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

struct Interned {
  llvm::DenseMap<const Type *, std::unique_ptr<Type>> optionals, arrays;
  using FunctionKey = std::tuple<std::vector<const Type *>, size_t, bool, const Type *, bool>;
  std::map<FunctionKey, std::unique_ptr<Type>> functions;
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
  std::vector<const Type *> atoms{nullptr};
  std::map<std::pair<double, const Type *>, std::unique_ptr<Type>> numbers;
  std::map<std::vector<const Type *>, std::unique_ptr<Type>> unions;
  std::map<const ClassDecl *, std::unique_ptr<Type>> classRefs;
  llvm::DenseMap<const Type *, std::unique_ptr<Type>> constructors;
};

Interned &Type::interned() {
  static Interned instance;

  return instance;
}

Type::ObjectKey Type::objectKey(llvm::ArrayRef<Field> fields, llvm::ArrayRef<Overload> overloads) {
  Interned::ObjectKey key;
  for (const Field &f : fields) {
    key.emplace_back(f.name, f.type, f.readonly ? 1 : 0);
  }
  for (const Overload &o : overloads) {
    for (const Type *signature : o.signatures) {
      key.emplace_back(o.name, signature, 2);
    }
  }
  llvm::sort(key);

  return key;
}

std::string Type::quotedText(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);
  std::string quoted = "\"";
  for (char c : utf8) {
    if (c == '\n') {
      quoted += "\\n";
    } else if (c == '\t') {
      quoted += "\\t";
    } else if (c == '"' || c == '\\') {
      quoted += std::string("\\") + c;
    } else {
      quoted += c;
    }
  }

  return quoted + "\"";
}

std::string Type::numberText(double value) {
  if (value == std::trunc(value) && std::fabs(value) < 1e21) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.0f", value);
    return buffer;
  }
  for (int precision = 1; precision <= 17; ++precision) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
    if (std::strtod(buffer, nullptr) == value) {
      return buffer;
    }
  }

  return std::to_string(value);
}

std::string Type::utf8Text(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);

  return utf8;
}

bool Type::isManaged() const {
  switch (this->kind()) {
    case Kind::Decimal:
    case Kind::String:
    case Kind::Array:
    case Kind::Function:
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
      return inner_->isManaged() || this->errType()->isManaged();
    case Kind::Union:
      return llvm::any_of(params_, [](const Type *m) { return m->isManaged(); });
    default:
      return false;
  }
}

bool Type::hasZeroValue() const {
  return isNumeric() || isDecimal() || isBool() || isString() || isOptional();
}

unsigned Type::bitWidth() const {
  switch (this->kind()) {
    case Kind::Int:
      return this->data_.as<IntData>()->bits;
    case Kind::Double:
      return 64;
    default:
      llvm_unreachable("bitWidth() of a non-numeric type");
  }
}

const Type *Type::error() {
  static const Type t(Kind::Error, "<error>");

  return &t;
}

const Type *Type::voidTy() {
  static const Type t(Kind::Void, "void");

  return &t;
}

const Type *Type::undefinedTy() {
  static const Type t(Kind::Undefined, "undefined");

  return &t;
}

const Type *Type::boolTy() {
  static const Type t(Kind::Bool, "boolean");

  return &t;
}

const Type *Type::intTy() { return integer(64, false); }

const Type *Type::integer(unsigned bits, bool isUnsigned) {
  llvm::ArrayRef<const Type *> all = integers();
  unsigned row = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;

  return all[row * 2 + (isUnsigned ? 1 : 0)];
}

llvm::ArrayRef<const Type *> Type::integers() {
  static std::vector<std::unique_ptr<Type>> owned;
  static const std::vector<const Type *> types = [] {
    std::vector<const Type *> made;
    for (unsigned bits : {8u, 16u, 32u, 64u}) {
      for (bool isUnsigned : {false, true}) {
        std::string name = bits == 64 && !isUnsigned
                               ? std::string("int")
                               : (isUnsigned ? "uint" : "int") + std::to_string(bits);
        Type *t = owned.emplace_back(new Type(Kind::Int, name)).get();
        t->data_ = IntData{.bits = bits, .isUnsigned = isUnsigned};
        made.push_back(t);
      }
    }

    return made;
  }();

  return types;
}

const Type *Type::wrapping(unsigned bits) {
  static const Type *types[2] = {[] {
                                   static Type t(Kind::Int, "Bit.Wrap32");
                                   t.data_ =
                                       IntData{.bits = 32, .isUnsigned = true, .wrapping = true};

                                   return &t;
                                 }(),
                                 [] {
                                   static Type t(Kind::Int, "Bit.Wrap64");
                                   t.data_ =
                                       IntData{.bits = 64, .isUnsigned = true, .wrapping = true};

                                   return &t;
                                 }()};

  return types[bits == 64 ? 1 : 0];
}

const Type *Type::doubleTy() {
  static const Type t(Kind::Double, "double");

  return &t;
}

const Type *Type::decimalTy() {
  static const Type t(Kind::Decimal, "decimal");

  return &t;
}

const Type *Type::string() {
  static const Type t(Kind::String, "string");

  return &t;
}

const Type *Type::never() {
  static const Type t(Kind::Never, "never");

  return &t;
}

const Type *Type::json() {
  static const Type t(Kind::Json, "Json");

  return &t;
}

const Type *Type::unknown() {
  static const Type t(Kind::Unknown, "unknown");

  return &t;
}

const Type *Type::optional(const Type *type) {
  if (type->isError() || type->isUndefined() || type->isOptional() || type->isUnknown()) {
    return type;
  }
  std::unique_ptr<Type> &slot = Type::interned().optionals[type];
  if (!slot) {
    std::string name = type->isFunction() ? "(" + type->name().str() + ")" : type->name().str();
    slot.reset(new Type(Kind::Optional, name + " | undefined", type));
  }

  return slot.get();
}

const Type *Type::array(const Type *element) {
  if (element->isError()) {
    return element;
  }
  std::unique_ptr<Type> &slot = Type::interned().arrays[element];
  if (!slot) {
    std::string name = element->isOptional() || element->isFunction() || element->isUnion()
                           ? "(" + element->name().str() + ")[]"
                           : element->name().str() + "[]";
    slot.reset(new Type(Kind::Array, std::move(name), element));
  }

  return slot.get();
}

const Type *Type::function(llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                           const Type *ret, bool hasThis) {
  for (const Type *t : params) {
    if (t->isError()) {
      return t;
    }
  }
  if (ret->isError()) {
    return ret;
  }
  std::vector<const Type *> flat;
  if (ArgumentLists::flattenArguments(params, minArgs, hasRest, flat)) {
    return function(flat, minArgs, hasRest, ret, hasThis);
  }
  Interned::FunctionKey key{std::vector<const Type *>(params.begin(), params.end()), minArgs,
                            hasRest, ret, hasThis};
  std::unique_ptr<Type> &slot = Type::interned().functions[key];
  if (!slot) {
    std::string name = "(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0) {
        name += ", ";
      }
      if (hasThis && i == 0) {
        name += "this: " + params[i]->name().str();
      } else if (hasRest && i + 1 == params.size()) {
        name += "..." + params[i]->name().str();
      } else if (i >= minArgs && params[i]->isOptional()) {
        name += params[i]->inner()->name().str() + "?";
      } else {
        name += params[i]->name().str();
      }
    }
    name += ") => " + ret->name().str();
    slot.reset(new Type(Kind::Function, std::move(name), ret));
    slot->params_.assign(params.begin(), params.end());
    slot->data_ = FunctionData{.minArgs = minArgs, .hasRest = hasRest, .hasThis = hasThis};
  }

  return slot.get();
}

const Field *Type::field(llvm::StringRef name) const {
  for (const Field &f : this->fields()) {
    if (f.name == name) {
      return &f;
    }
  }

  return nullptr;
}

const Overload *Type::overload(llvm::StringRef name) const {
  for (const Overload &o : this->overloads()) {
    if (o.name == name) {
      return &o;
    }
  }

  return nullptr;
}

const Type *Type::object(std::vector<Field> fields, llvm::StringRef name,
                         std::vector<Overload> overloads) {
  for (const Field &f : fields) {
    if (f.type->isError()) {
      return f.type;
    }
  }
  const Type *&slot = Type::interned().objectsByKey[Type::objectKey(fields, overloads)];
  if (!slot) {
    std::string spelling = name.empty() ? TypeSpelling::objectTypeSpelling(fields) : name.str();
    auto &type = Type::interned().objects.emplace_back(new Type(Kind::Object, std::move(spelling)));
    ObjectData *shape = type->data_.as<ObjectData>();
    shape->fields = std::move(fields);
    shape->overloads = std::move(overloads);
    slot = type.get();
  }

  return slot;
}

const Type *Type::tuple(llvm::ArrayRef<const Type *> elements, bool readonly) {
  for (const Type *t : elements) {
    if (t->isError()) {
      return t;
    }
  }
  const Type *&slot =
      Type::interned()
          .tuples[{std::vector<const Type *>(elements.begin(), elements.end()), readonly}];
  if (!slot) {
    std::string name = readonly ? "readonly [" : "[";
    std::vector<Field> fields;
    for (size_t i = 0; i < elements.size(); ++i) {
      if (i > 0) {
        name += ", ";
      }
      name += (elements[i]->isListParam() ? "..." : "") + elements[i]->name().str();
      fields.push_back({std::to_string(i), elements[i], readonly});
    }
    auto &type = Type::interned().objects.emplace_back(new Type(Kind::Object, name + "]"));
    ObjectData *shape = type->data_.as<ObjectData>();
    shape->fields = std::move(fields);
    shape->tuple = true;
    shape->tupleMin = elements.size();
    slot = type.get();
  }

  return slot;
}

const Type *Type::argumentList(llvm::ArrayRef<const Type *> elements, size_t min,
                               const Type *rest) {
  if (min >= elements.size() && !rest) {
    return tuple(elements);
  }
  for (const Type *t : elements) {
    if (t->isError()) {
      return t;
    }
  }
  if (rest && rest->isError()) {
    return rest;
  }
  const Type *&slot =
      Type::interned()
          .lists[{std::vector<const Type *>(elements.begin(), elements.end()), min, rest}];
  if (!slot) {
    std::string name = "[";
    std::vector<Field> fields;
    for (size_t i = 0; i < elements.size(); ++i) {
      if (i > 0) {
        name += ", ";
      }
      name += i >= min && elements[i]->isOptional() ? elements[i]->inner()->name().str() + "?"
                                                    : elements[i]->name().str();
      fields.push_back({std::to_string(i), elements[i], false});
    }
    if (rest) {
      name += (elements.empty() ? "..." : ", ...") + rest->name().str();
    }
    auto &type = Type::interned().objects.emplace_back(new Type(Kind::Object, name + "]"));
    ObjectData *shape = type->data_.as<ObjectData>();
    shape->fields = std::move(fields);
    shape->tuple = true;
    shape->tupleMin = min;
    shape->tupleRest = rest;
    slot = type.get();
  }

  return slot;
}

const Type *Type::result(const Type *ok, const Type *err) {
  if (ok->isError()) {
    return ok;
  }
  if (err->isError()) {
    return err;
  }
  if (ok->isUndefined()) {
    ok = voidTy();
  }
  if (err->isUndefined()) {
    err = voidTy();
  }
  std::unique_ptr<Type> &slot = Type::interned().results[{ok, err}];
  if (!slot) {
    std::string name = "Result<" + ok->name().str() + ", " + err->name().str() + ">";
    slot.reset(new Type(Kind::Result, std::move(name), ok));
    slot->data_ = ResultData{.error = err};
  }

  return slot.get();
}

const Type *Type::iterator(const Type *yielded, const Type *returned, const Type *sent) {
  for (const Type *t : {yielded, returned, sent}) {
    if (t->isError()) {
      return t;
    }
  }
  std::unique_ptr<Type> &slot = Type::interned().iterators[{yielded, returned, sent}];
  if (!slot) {
    std::string name = "Generator<" + yielded->name().str();
    if (!returned->isVoid() || !sent->isVoid()) {
      name += ", " + returned->name().str();
    }
    if (!sent->isVoid()) {
      name += ", " + sent->name().str();
    }
    slot.reset(new Type(Kind::Iterator, name + ">", yielded));
    slot->params_ = {returned, sent};
  }

  return slot.get();
}

const Type *Type::dict(DictKind kind, const Type *key, const Type *value) {
  if (key->isError()) {
    return key;
  }
  if (value->isError()) {
    return value;
  }
  std::unique_ptr<Type> &slot = Type::interned().dicts[{int(kind), key, value}];
  if (!slot) {
    std::string name = kind == DictKind::Record ? "Record<"
                       : kind == DictKind::Map  ? "Map<"
                                                : "Set<";
    name += key->name().str();
    if (kind != DictKind::Set) {
      name += ", " + value->name().str();
    }
    slot.reset(new Type(Kind::Dict, name + ">", value));
    slot->params_ = {key};
    slot->data_ = DictData{.dictKind = kind};
  }

  return slot.get();
}

const Type *Type::stringPattern(std::vector<std::u16string> texts,
                                std::vector<const Type *> holes) {
  if (holes.size() == 1 && holes[0]->isString() && texts[0].empty() && texts[1].empty()) {
    return holes[0];
  }
  std::unique_ptr<Type> &slot = Type::interned().patterns[{texts, holes}];
  if (!slot) {
    std::string name = "`";
    for (size_t i = 0; i < texts.size(); ++i) {
      name += Type::utf8Text(texts[i]);
      if (i < holes.size()) {
        name += "${" + (holes[i]->isFloat() ? std::string("number") : holes[i]->name().str()) + "}";
      }
    }
    slot.reset(new Type(Kind::String, name + "`"));
    slot->data_ = StringData{.pattern = true, .texts = std::move(texts)};
    slot->params_ = std::move(holes);
  }

  return slot.get();
}

const Type *Type::stringLiteral(const std::u16string &text) {
  std::unique_ptr<Type> &slot = Type::interned().strings[text];
  if (!slot) {
    slot.reset(new Type(Kind::Literal, Type::quotedText(text), string()));
    unsigned index = unsigned(Type::interned().atoms.size());
    slot->data_ = LiteralData{.text = text, .atom = index};
    Type::interned().atoms.push_back(slot.get());
  }

  return slot.get();
}

const Type *Type::numberLiteral(double value, const Type *base) {
  std::unique_ptr<Type> &slot = Type::interned().numbers[{value, base}];
  if (!slot) {
    slot.reset(new Type(Kind::Literal, Type::numberText(value), base));
    slot->data_ = LiteralData{.number = value};
  }

  return slot.get();
}

const Type *Type::boolLiteral(bool value) {
  static const Type *literals[2] = {nullptr, nullptr};
  const Type *&slot = literals[value ? 1 : 0];
  if (!slot) {
    auto *literal = new Type(Kind::Literal, value ? "true" : "false", boolTy());
    Type *type = Type::interned().objects.emplace_back(literal).get();
    type->data_ = LiteralData{.number = value ? 1.0 : 0.0};
    slot = type;
  }

  return slot;
}

llvm::ArrayRef<const Type *> Type::stringLiterals() {
  return llvm::ArrayRef<const Type *>(Type::interned().atoms).drop_front();
}

const Type *Type::slotPart(UnionSlot slot) const {
  if (!isUnion()) {
    return UnionSlots::unionSlot(this) == slot ? this : nullptr;
  }
  std::vector<const Type *> part;
  for (const Type *m : params_) {
    if (UnionSlots::unionSlot(m) == slot) {
      part.push_back(m);
    }
  }
  if (part.empty()) {
    return nullptr;
  }

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
      for (const Type *m : t->members()) {
        add(m);
      }
      return true;
    default:
      if (!llvm::is_contained(members, t)) {
        members.push_back(t);
      }
      return true;
    }
  };
  for (const Type *t : types) {
    if (!add(t)) {
      return t;
    }
  }
  auto has = [&](const Type *t) { return llvm::is_contained(members, t); };
  if (has(unknown())) {
    return unknown();
  }
  if (has(boolLiteral(true)) && has(boolLiteral(false))) {
    llvm::erase(members, boolLiteral(true));
    llvm::erase(members, boolLiteral(false));
    if (!has(boolTy())) {
      members.push_back(boolTy());
    }
  }
  llvm::erase_if(members, [&](const Type *m) {
    if (!m->isLiteral()) {
      return false;
    }
    const Type *base = m->literalBase();
    if (base->isInteger()) {
      return has(base) || has(doubleTy());
    }

    return has(base);
  });
  if (has(string())) {
    llvm::erase_if(members, [](const Type *m) { return m->isPattern(); });
  }
  llvm::erase_if(members, [&](const Type *m) {
    return m->isStringLiteral() && llvm::any_of(members, [&](const Type *p) {
             return p->isPattern() && TemplatePatterns::matchesPattern(m->literalText(), p);
           });
  });
  bool fractions = llvm::any_of(
      members, [](const Type *m) { return m->isLiteral() && m->literalBase()->isFloat(); });
  if (fractions) {
    for (const Type *&m : members) {
      if (m->isLiteral() && m->literalBase()->isInteger()) {
        m = numberLiteral(m->literalNumber(), doubleTy());
      }
    }
  }
  if (llvm::any_of(members, [](const Type *m) { return m->isAnyObject(); })) {
    llvm::erase_if(members, [](const Type *m) { return m->isObject() && !m->isAnyObject(); });
  }
  llvm::erase_if(members, [](const Type *m) {
    return m->isObject() && !m->isClass() && !m->isTuple() &&
           llvm::any_of(m->fields(), [](const Field &f) { return f.type->isNever(); });
  });
  std::vector<const Type *> reduced;
  for (const Type *m : members) {
    if (!m->isObject() || llvm::none_of(members, [&](const Type *o) {
          return o != m && o->isObject() && m->isSubtypeOf(o);
        })) {
      if (!llvm::is_contained(reduced, m)) {
        reduced.push_back(m);
      }
    }
  }
  members = std::move(reduced);

  const Type *result;
  if (members.empty()) {
    return undefined ? undefinedTy() : never();
  }
  if (members.size() == 1) {
    result = members.front();
  } else {
    std::vector<const Type *> key = members;
    llvm::sort(key, [](const Type *a, const Type *b) { return a->id() < b->id(); });
    std::unique_ptr<Type> &slot = Type::interned().unions[key];
    if (!slot) {
      std::string name;
      for (const Type *m : members) {
        name += (name.empty() ? "" : " | ") +
                (m->isFunction() ? "(" + m->name().str() + ")" : m->name().str());
      }
      slot.reset(new Type(Kind::Union, std::move(name)));
      slot->params_ = members;
      UnionData *joined = slot->data_.as<UnionData>();
      for (const Type *m : members) {
        UnionSlot s = UnionSlots::unionSlot(m);
        if (!llvm::is_contained(joined->slots, s)) {
          joined->slots.push_back(s);
        }
      }
      llvm::sort(joined->slots);
    }
    result = slot.get();
  }

  return undefined ? optional(result) : result;
}

const Type *Type::promise(const Type *value) {
  if (value->isError()) {
    return value;
  }
  if (value->isUndefined()) {
    value = voidTy();
  }
  std::unique_ptr<Type> &slot = Type::interned().promises[value];
  if (!slot) {
    slot.reset(new Type(Kind::Promise, "Promise<" + value->name().str() + ">", value));
  }

  return slot.get();
}

Type *Type::declareObject(llvm::StringRef name) {
  auto &type = Type::interned().objects.emplace_back(new Type(Kind::Object, name.str()));
  type->incomplete_ = true;

  return type.get();
}

Type *Type::classType(llvm::StringRef name, const ClassDecl *decl) {
  auto &type = Type::interned().objects.emplace_back(new Type(Kind::Object, name.str()));
  type->incomplete_ = true;
  type->data_.as<ObjectData>()->cls = decl;

  return type.get();
}

Type *Type::declareUnion(llvm::StringRef name) {
  auto &type = Type::interned().objects.emplace_back(new Type(Kind::Union, name.str()));
  type->incomplete_ = true;

  return type.get();
}

void Type::nameUnion(const Type *type, llvm::StringRef name) {
  if (type->isUnion()) {
    const_cast<Type *>(type)->name_ = name.str();
  }
}

void Type::defineMembers(const Type *of) {
  params_.assign(of->params_.begin(), of->params_.end());
  this->data_.as<UnionData>()->slots = of->data_.as<UnionData>()->slots;
  incomplete_ = false;
}

void Type::defineFields(std::vector<Field> fields, std::vector<Overload> overloads) {
  ObjectData *shape = this->data_.as<ObjectData>();
  shape->fields = std::move(fields);
  shape->overloads = std::move(overloads);
  incomplete_ = false;
}

void Type::addBase(const Type *base) {
  if (base == this || llvm::is_contained(bases_, base)) {
    return;
  }
  bases_.push_back(base);
  const_cast<Type *>(base)->isBase_ = true;
}

void Type::swapClasses(Type *a, Type *b) {
  ObjectData *first = a->data_.as<ObjectData>();
  ObjectData *second = b->data_.as<ObjectData>();
  std::swap(first->cls, second->cls);
  std::swap(first->fields, second->fields);
  std::swap(first->overloads, second->overloads);
  std::swap(a->incomplete_, b->incomplete_);
  std::swap(a->bases_, b->bases_);
  std::swap(a->isBase_, b->isBase_);
  for (Type *t : {a, b}) {
    for (const Type *&base : t->bases_) {
      base = base == a ? b : base == b ? a : base;
    }
  }
}

bool Type::isSubtypeOf(const Type *base) const {
  if (this == base) {
    return true;
  }
  for (const Type *b : bases_) {
    if (b->isSubtypeOf(base)) {
      return true;
    }
  }

  return false;
}

Type *Type::param(llvm::StringRef name) {
  return Type::interned().objects.emplace_back(new Type(Kind::Param, name.str())).get();
}

const Type *Type::typeOp(TypeOp op, llvm::ArrayRef<const Type *> operands) {
  const Type *&slot =
      Type::interned()
          .ops[{int(op), std::vector<const Type *>(operands.begin(), operands.end()), 0, 0}];
  if (!slot) {
    std::string name;
    auto of = [&](size_t i) {
      return i < operands.size() && operands[i] ? operands[i]->name().str() : "";
    };
    switch (op) {
    case TypeOp::KeyOf:
      name = "keyof " + of(0);
      break;
    case TypeOp::Index:
      name = of(0) + "[" + (operands[1]->isInt() ? "number" : of(1)) + "]";
      break;
    case TypeOp::Exclude:
      name = "Exclude<" + of(0) + ", " + of(1) + ">";
      break;
    case TypeOp::Extract:
      name = "Extract<" + of(0) + ", " + of(1) + ">";
      break;
    case TypeOp::ReturnType:
      name = "ReturnType<" + of(0) + ">";
      break;
    case TypeOp::Parameters:
      name = "Parameters<" + of(0) + ">";
      break;
    case TypeOp::NonNullable:
      name = "NonNullable<" + of(0) + ">";
      break;
    case TypeOp::Conditional:
      name = of(0);
      break;
    case TypeOp::Template:
      name = "`";
      for (size_t i = 0; i < operands.size(); ++i) {
        name += i % 2 == 0 ? Type::utf8Text(operands[i]->literalText()) : "${" + of(i) + "}";
      }
      name += "`";
      break;
    case TypeOp::Uppercase:
      name = "Uppercase<" + of(0) + ">";
      break;
    case TypeOp::Lowercase:
      name = "Lowercase<" + of(0) + ">";
      break;
    case TypeOp::Capitalize:
      name = "Capitalize<" + of(0) + ">";
      break;
    case TypeOp::Uncapitalize:
      name = "Uncapitalize<" + of(0) + ">";
      break;
    case TypeOp::Spread:
      name = "[";
      for (size_t i = 0; i + 1 < operands.size(); i += 2) {
        name += (i ? ", " : "") + std::string(operands[i]->literalNumber() != 0 ? "..." : "") +
                of(i + 1);
      }
      name += "]";
      break;
    default:
      name = "?";
      break;
    }
    Type *type = param(name);
    ParamData *operation = type->data_.as<ParamData>();
    operation->op = op;
    operation->operands.assign(operands.begin(), operands.end());
    slot = type;
  }

  return slot;
}

const Type *Type::mapped(const Type *key, const Type *keys, const Type *value, int optional,
                         int readonly, const Type *source, const Type *rename) {
  const Type *&slot =
      Type::interned()
          .ops[{int(TypeOp::Mapped), {key, keys, value, source, rename}, optional, readonly}];
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
    ParamData *operation = type->data_.as<ParamData>();
    operation->op = TypeOp::Mapped;
    operation->operands = {key, keys, value, source, rename};
    operation->mappedOptional = optional;
    operation->mappedReadonly = readonly;
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
    Type *t = Type::interned().objects.emplace_back(new Type(Kind::Object, "object")).get();
    t->data_.as<ObjectData>()->form = AnyObject{};

    return t;
  }();

  return type;
}

const Type *Type::classRef(const ClassDecl *cls, llvm::StringRef name) {
  std::unique_ptr<Type> &slot = Type::interned().classRefs[cls];
  if (!slot) {
    slot.reset(new Type(Kind::ClassRef, "typeof " + name.str()));
    slot->data_ = ClassRefData{.refClass = cls};
  }

  return slot.get();
}

const Type *Type::constructorOf(const Type *instance) {
  if (instance->isError()) {
    return instance;
  }
  std::unique_ptr<Type> &slot = Type::interned().constructors[instance];
  if (!slot) {
    slot.reset(new Type(Kind::ClassRef,
                        instance->isAnyObject() ? std::string("Constructor")
                                                : "Constructor<" + instance->name().str() + ">",
                        instance));
  }

  return slot.get();
}

const Type *Type::regExp() {
  static Type *type = [] {
    Type *t = Type::interned().objects.emplace_back(new Type(Kind::Object, "RegExp")).get();
    ObjectData *shape = t->data_.as<ObjectData>();
    shape->fields.push_back({"lastIndex", intTy(), false});
    shape->form = RegExpObject{};

    return t;
  }();

  return type;
}

const Type *Type::date() {
  static Type *type = [] {
    Type *t = Type::interned().objects.emplace_back(new Type(Kind::Object, "Date")).get();
    ObjectData *shape = t->data_.as<ObjectData>();
    shape->fields.push_back({"%time", intTy(), false});
    shape->form = DateObject{};

    return t;
  }();

  return type;
}

const Type *Type::weakRef(const Type *target) {
  static std::map<const Type *, Type *> types;
  Type *&type = types[target];
  if (!type) {
    type =
        Type::interned()
            .objects
            .emplace_back(new Type(Kind::Object, "WeakRef<" + target->name().str() + ">", target))
            .get();
    type->data_.as<ObjectData>()->form = WeakRefObject{};
  }

  return type;
}

const Type *Type::dateFormat() { return intl(IntlKind::DateTimeFormat); }

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
    type = Type::interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->data_.as<ObjectData>()->form = IntlObject{.intl = kind};
  }

  return type;
}

const Type *Type::timeout() {
  static Type *type = [] {
    Type *t = Type::interned().objects.emplace_back(new Type(Kind::Object, "Timeout")).get();
    t->data_.as<ObjectData>()->form = TimeoutObject{};

    return t;
  }();

  return type;
}

const Type *Type::immediate() {
  static Type *type = [] {
    Type *t = Type::interned().objects.emplace_back(new Type(Kind::Object, "Immediate")).get();
    t->data_.as<ObjectData>()->form = ImmediateObject{};

    return t;
  }();

  return type;
}

const Type *Type::cryptoType(Crypto kind) {
  static const char *const names[] = {"",          "Hash", "Hmac",   "Cipheriv", "Decipheriv",
                                      "KeyObject", "Sign", "Verify", "ECDH",     "X509Certificate"};
  static Type *types[std::size(names)] = {};
  Type *&type = types[size_t(kind)];
  if (!type) {
    type = Type::interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->data_.as<ObjectData>()->form = CryptoObject{.crypto = kind};
  }

  return type;
}

const Type *Type::bytesType(Bytes kind) {
  static const char *const names[] = {"",
                                      "Uint8Array",
                                      "Int8Array",
                                      "Uint8ClampedArray",
                                      "Int16Array",
                                      "Uint16Array",
                                      "Int32Array",
                                      "Uint32Array",
                                      "Float16Array",
                                      "Float32Array",
                                      "Float64Array",
                                      "Buffer",
                                      "ArrayBuffer",
                                      "DataView",
                                      "TextEncoder",
                                      "TextDecoder",
                                      "FatalTextDecoder"};
  static Type *types[std::size(names)] = {};
  Type *&type = types[size_t(kind)];
  if (!type) {
    type = Type::interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->data_.as<ObjectData>()->form = BytesObject{.bytes = kind};
  }

  return type;
}

const Type *Type::runtimeObject(Runtime kind) {
  static Type *types[size_t(Runtime::AsyncIterator)];
  static const char *const names[] = {"", "FileHandle", "Interface", "Interface"};
  Type *&type = types[size_t(kind)];
  if (!type) {
    type = Type::interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->data_.as<ObjectData>()->form = RuntimeObject{.runtime = kind};
  }

  return type;
}

const Type *Type::elementType() const {
  switch (this->bytes()) {
    case Bytes::Float16:
    case Bytes::Float32:
    case Bytes::Float64:
      return doubleTy();
    case Bytes::Int8:
      return integer(8, false);
    case Bytes::Int16:
      return integer(16, false);
    case Bytes::Uint16:
      return integer(16, true);
    case Bytes::Int32:
      return integer(32, false);
    case Bytes::Uint32:
      return integer(32, true);
    default:
      return integer(8, true);
  }
}

const Type *Type::accessType() const { return elementType()->isFloat() ? doubleTy() : intTy(); }

unsigned Type::elementSize() const {
  switch (this->bytes()) {
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
  switch (this->bytes()) {
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
  for (const Type *t : {yielded, returned, sent}) {
    if (t->isError()) {
      return t;
    }
  }
  const Type *&slot = Type::interned().asyncIterators[{yielded, returned, sent}];
  if (!slot) {
    std::string name = "AsyncGenerator<" + yielded->name().str();
    if (!returned->isVoid() || !sent->isVoid()) {
      name += ", " + returned->name().str();
    }
    if (!sent->isVoid()) {
      name += ", " + sent->name().str();
    }
    Type *type =
        Type::interned().objects.emplace_back(new Type(Kind::Object, name + ">", yielded)).get();
    type->params_ = {returned, sent};
    type->data_.as<ObjectData>()->form = RuntimeObject{.runtime = Runtime::AsyncIterator};
    slot = type;
  }

  return slot;
}

const Type *Type::match(int groups, std::vector<bool> mandatory, const Type *named, bool indices) {
  if (groups < 0) {
    mandatory.clear();
  } else {
    mandatory.resize(size_t(groups) + 1, false);
  }
  const Type *&slot = Type::interned().matches[{groups, mandatory, named, indices}];
  if (slot) {
    return slot;
  }
  std::string name = "RegExpMatchArray";
  if (groups >= 0) {
    name += " [string";
    for (int k = 1; k <= groups; ++k) {
      name += mandatory[size_t(k)] ? ", string" : ", string | undefined";
    }
    name += "]";
    if (named) {
      name += " with groups " + named->name().str();
    }
  }
  Type *type = Type::interned().objects.emplace_back(new Type(Kind::Object, name)).get();
  ObjectData *shape = type->data_.as<ObjectData>();
  shape->fields.push_back({"%items", array(optional(string())), true});
  shape->fields.push_back({"index", intTy(), false});
  shape->fields.push_back({"input", string(), false});
  shape->fields.push_back({"groups", named ? named : optional(object({})), false});
  if (indices) {
    shape->fields.push_back({"indices", array(optional(tuple({intTy(), intTy()}))), false});
  }
  shape->form = MatchObject{.groups = groups, .mandatory = std::move(mandatory)};
  slot = type;

  return slot;
}

const Type *Type::fromName(llvm::StringRef name) {
  return llvm::StringSwitch<const Type *>(name)
      .Case("void", voidTy())
      .Case("undefined", undefinedTy())
      .Case("boolean", boolTy())
      .Case("int", intTy())
      .Case("int8", integer(8, false))
      .Case("int16", integer(16, false))
      .Case("int32", integer(32, false))
      .Case("uint8", integer(8, true))
      .Case("uint16", integer(16, true))
      .Case("uint32", integer(32, true))
      .Case("uint64", integer(64, true))
      .Case("Bit.Wrap32", wrapping(32))
      .Case("Bit.Wrap64", wrapping(64))
      .Cases({"double", "number"}, doubleTy())
      .Case("decimal", decimalTy())
      .Case("string", string())
      .Case("Json", json())
      .Case("unknown", unknown())
      .Case("object", anyObject())
      .Default(nullptr);
}

bool Type::isTimer() const {
  return this->objectForm()->is<TimeoutObject>() || this->objectForm()->is<ImmediateObject>();
}

bool Type::isImmediate() const {
  return this->objectForm()->is<ImmediateObject>();
}

Type::Runtime Type::runtimeKind() const {
  const RuntimeObject *handle = this->objectForm()->as<RuntimeObject>();

  return handle ? handle->runtime : Runtime::None;
}

bool Type::isRuntimeObject() const {
  return this->objectForm()->is<RuntimeObject>();
}

bool Type::isAsyncIterator() const {
  return this->runtimeKind() == Runtime::AsyncIterator;
}

Type::Crypto Type::crypto() const {
  const CryptoObject *made = this->objectForm()->as<CryptoObject>();

  return made ? made->crypto : Crypto::None;
}

bool Type::isCrypto() const {
  return this->objectForm()->is<CryptoObject>();
}

bool Type::isAnyObject() const {
  return this->objectForm()->is<AnyObject>();
}

bool Type::isRegExp() const {
  return this->objectForm()->is<RegExpObject>();
}

bool Type::isMatch() const {
  return this->objectForm()->is<MatchObject>();
}

bool Type::isDate() const {
  return this->objectForm()->is<DateObject>();
}

bool Type::isDateFormat() const {
  return this->intlKind() == IntlKind::DateTimeFormat;
}

bool Type::isIntl() const {
  return this->objectForm()->is<IntlObject>();
}

IntlKind Type::intlKind() const {
  const IntlObject *format = this->objectForm()->as<IntlObject>();

  return format ? format->intl : IntlKind::None;
}

bool Type::isWeakRef() const {
  return this->objectForm()->is<WeakRefObject>();
}

const Type *Type::weakTarget() const {
  return this->isWeakRef() ? this->inner_ : nullptr;
}

Type::Bytes Type::bytes() const {
  const BytesObject *view = this->objectForm()->as<BytesObject>();

  return view ? view->bytes : Bytes::None;
}

bool Type::isBytes() const {
  return this->objectForm()->is<BytesObject>();
}

bool Type::isTypedArray() const {
  Bytes element = this->bytes();

  return element >= Bytes::Uint8 && element <= Bytes::Buffer;
}

bool Type::isBuffer() const {
  return this->bytes() == Bytes::Buffer;
}

bool Type::isByteView() const {
  return this->isTypedArray() || this->bytes() == Bytes::DataView;
}

int Type::matchGroups() const {
  const MatchObject *found = this->objectForm()->as<MatchObject>();

  return found ? found->groups : -1;
}

bool Type::captureMandatory(unsigned k) const {
  const MatchObject *found = this->objectForm()->as<MatchObject>();

  return k == 0 || (found && k < found->mandatory.size() && found->mandatory[k]);
}

Type::Data Type::dataFor(Kind kind) {
  switch (kind) {
    case Kind::Error:
      return ErrorData{};
    case Kind::Void:
      return VoidData{};
    case Kind::Undefined:
      return UndefinedData{};
    case Kind::Bool:
      return BoolData{};
    case Kind::Int:
      return IntData{};
    case Kind::Double:
      return DoubleData{};
    case Kind::Decimal:
      return DecimalData{};
    case Kind::String:
      return StringData{};
    case Kind::Optional:
      return OptionalData{};
    case Kind::Array:
      return ArrayData{};
    case Kind::Function:
      return FunctionData{};
    case Kind::Object:
      return ObjectData{};
    case Kind::Param:
      return ParamData{};
    case Kind::Result:
      return ResultData{};
    case Kind::Never:
      return NeverData{};
    case Kind::Iterator:
      return IteratorData{};
    case Kind::Json:
      return JsonData{};
    case Kind::Dict:
      return DictData{};
    case Kind::Literal:
      return LiteralData{};
    case Kind::Union:
      return UnionData{};
    case Kind::Promise:
      return PromiseData{};
    case Kind::Unknown:
      return UnknownData{};
    case Kind::ClassRef:
      return ClassRefData{};
  }

  return ErrorData{};
}

const Type::ObjectForm *Type::objectForm() const {
  static const ObjectForm ordinary = OrdinaryObject{};
  const ObjectData *shape = this->data_.as<ObjectData>();

  return shape ? &shape->form : &ordinary;
}

bool Type::isInt() const {
  const IntData *number = this->data_.as<IntData>();

  return number && number->bits == 64 && !number->isUnsigned;
}

bool Type::isUnsigned() const {
  const IntData *number = this->data_.as<IntData>();

  return number && number->isUnsigned;
}

bool Type::isWrapping() const {
  const IntData *number = this->data_.as<IntData>();

  return number && number->wrapping;
}

int64_t Type::intMin() const {
  const IntData *number = this->data_.as<IntData>();
  unsigned bits = number ? number->bits : 0;
  bool unsignedInt = number && number->isUnsigned;

  return unsignedInt ? 0 : bits == 64 ? INT64_MIN : -(int64_t(1) << (bits - 1));
}

uint64_t Type::intMax() const {
  const IntData *number = this->data_.as<IntData>();
  unsigned bits = number ? number->bits : 0;
  bool unsignedInt = number && number->isUnsigned;

  return bits == 64 ? (unsignedInt ? UINT64_MAX : uint64_t(INT64_MAX))
                    : (uint64_t(1) << (unsignedInt ? bits : bits - 1)) - 1;
}

bool Type::isTuple() const {
  const ObjectData *shape = this->data_.as<ObjectData>();

  return shape && shape->tuple;
}

bool Type::isClass() const {
  return this->classDecl() != nullptr;
}

const ClassDecl *Type::classDecl() const {
  const ObjectData *shape = this->data_.as<ObjectData>();

  return shape ? shape->cls : nullptr;
}

const ClassDecl *Type::classRef() const {
  const ClassRefData *ref = this->data_.as<ClassRefData>();

  return ref ? ref->refClass : nullptr;
}

bool Type::isConstructor() const {
  const ClassRefData *ref = this->data_.as<ClassRefData>();

  return ref && !ref->refClass;
}

bool Type::isRecord() const {
  const DictData *table = this->data_.as<DictData>();

  return table && table->dictKind == DictKind::Record;
}

bool Type::isMap() const {
  const DictData *table = this->data_.as<DictData>();

  return table && table->dictKind == DictKind::Map;
}

bool Type::isSet() const {
  const DictData *table = this->data_.as<DictData>();

  return table && table->dictKind == DictKind::Set;
}

Type::DictKind Type::dictKind() const {
  const DictData *table = this->data_.as<DictData>();

  return table ? table->dictKind : DictKind::Record;
}

const std::u16string &Type::literalText() const {
  static const std::u16string none;
  const LiteralData *literal = this->data_.as<LiteralData>();

  return literal ? literal->text : none;
}

double Type::literalNumber() const {
  const LiteralData *literal = this->data_.as<LiteralData>();

  return literal ? literal->number : 0;
}

unsigned Type::atom() const {
  const LiteralData *literal = this->data_.as<LiteralData>();

  return literal ? literal->atom : 0;
}

llvm::ArrayRef<UnionSlot> Type::slots() const {
  const UnionData *joined = this->data_.as<UnionData>();

  return joined ? llvm::ArrayRef<UnionSlot>(joined->slots) : llvm::ArrayRef<UnionSlot>();
}

bool Type::isTagged() const {
  return this->slots().size() > 1;
}

size_t Type::minArgs() const {
  const FunctionData *signature = this->data_.as<FunctionData>();

  return signature ? signature->minArgs : 0;
}

bool Type::hasRest() const {
  const FunctionData *signature = this->data_.as<FunctionData>();

  return signature && signature->hasRest;
}

bool Type::hasThis() const {
  const FunctionData *signature = this->data_.as<FunctionData>();

  return signature && signature->hasThis;
}

const Type *Type::errType() const {
  const ResultData *outcome = this->data_.as<ResultData>();

  return outcome ? outcome->error : nullptr;
}

llvm::ArrayRef<Field> Type::fields() const {
  const ObjectData *shape = this->data_.as<ObjectData>();

  return shape ? llvm::ArrayRef<Field>(shape->fields) : llvm::ArrayRef<Field>();
}

llvm::ArrayRef<Overload> Type::overloads() const {
  const ObjectData *shape = this->data_.as<ObjectData>();

  return shape ? llvm::ArrayRef<Overload>(shape->overloads) : llvm::ArrayRef<Overload>();
}

const Type *Type::iterated() const {
  const ParamData *parameter = this->data_.as<ParamData>();

  return parameter ? parameter->iterated : nullptr;
}

void Type::setIterated(const Type *element) {
  if (ParamData *parameter = this->data_.as<ParamData>()) {
    parameter->iterated = element;
  }
}

bool Type::isListParam() const {
  const ParamData *parameter = this->data_.as<ParamData>();

  return parameter && parameter->list;
}

void Type::setList() {
  if (ParamData *parameter = this->data_.as<ParamData>()) {
    parameter->list = true;
  }
}

TypeOp Type::typeOp() const {
  const ParamData *parameter = this->data_.as<ParamData>();

  return parameter ? parameter->op : TypeOp::None;
}

llvm::ArrayRef<const Type *> Type::operands() const {
  const ParamData *parameter = this->data_.as<ParamData>();

  return parameter ? llvm::ArrayRef<const Type *>(parameter->operands)
                   : llvm::ArrayRef<const Type *>();
}

int Type::mappedOptional() const {
  const ParamData *parameter = this->data_.as<ParamData>();

  return parameter ? parameter->mappedOptional : 0;
}

int Type::mappedReadonly() const {
  const ParamData *parameter = this->data_.as<ParamData>();

  return parameter ? parameter->mappedReadonly : 0;
}

size_t Type::tupleMin() const {
  const ObjectData *shape = this->data_.as<ObjectData>();

  return shape ? shape->tupleMin : 0;
}

const Type *Type::tupleRest() const {
  const ObjectData *shape = this->data_.as<ObjectData>();

  return shape ? shape->tupleRest : nullptr;
}

void Type::forgetFields() {
  if (ObjectData *shape = this->data_.as<ObjectData>()) {
    shape->fields.clear();
  }
  incomplete_ = true;
}

llvm::ArrayRef<const Type *> Type::typeArgs() const {
  const ObjectData *shape = this->data_.as<ObjectData>();

  return shape ? llvm::ArrayRef<const Type *>(shape->typeArgs) : llvm::ArrayRef<const Type *>();
}

void Type::setTypeArgs(std::vector<const Type *> args) {
  if (ObjectData *shape = this->data_.as<ObjectData>()) {
    shape->typeArgs = std::move(args);
  }
}

bool Type::isPattern() const {
  const StringData *text = this->data_.as<StringData>();

  return text && text->pattern;
}

llvm::ArrayRef<std::u16string> Type::patternTexts() const {
  const StringData *text = this->data_.as<StringData>();

  return text ? llvm::ArrayRef<std::u16string>(text->texts) : llvm::ArrayRef<std::u16string>();
}

llvm::ArrayRef<const Type *> Type::patternHoles() const {
  return this->isPattern() ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
}

bool Type::holdsFunctions() const {
  const Type *held = this;
  while (held->isOptional() || held->isArray()) {
    held = held->inner();
  }

  return held->isFunction();
}

bool Type::holdsObjects() const {
  const Type *held = this;
  while (held->isOptional() || held->isArray()) {
    held = held->inner();
  }

  return held->isObject() || held->isDict();
}

bool Type::holdsDates() const {
  const Type *held = this;
  while (held->isOptional() || held->isArray()) {
    held = held->inner();
  }

  return held->isDate();
}

bool Type::holdsResults() const {
  const Type *held = this;
  while (held->isOptional() || held->isArray()) {
    held = held->inner();
  }

  return held->isResult();
}

bool Type::holdsPromises() const {
  const Type *held = this;
  while (held->isOptional() || held->isArray()) {
    held = held->inner();
  }

  return held->isPromise();
}

}
