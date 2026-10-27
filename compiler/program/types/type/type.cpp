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

bool Type::isManaged() const {
  switch (kind_) {
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
    return inner_->isManaged() || error_->isManaged();
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
  switch (kind_) {
  case Kind::Int:
    return 32;
  case Kind::Long:
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

const Type *Type::intTy() {
  static const Type t(Kind::Int, "int");

  return &t;
}

const Type *Type::longTy() {
  static const Type t(Kind::Long, "long");

  return &t;
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

Interned &interned() {
  static Interned instance;

  return instance;
}

const Type *Type::optional(const Type *type) {
  if (type->isError() || type->isUndefined() || type->isOptional() || type->isUnknown()) {
    return type;
  }
  std::unique_ptr<Type> &slot = interned().optionals[type];
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
  std::unique_ptr<Type> &slot = interned().arrays[element];
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
  if (flattenArguments(params, minArgs, hasRest, flat)) {
    return function(flat, minArgs, hasRest, ret, hasThis);
  }
  Interned::FunctionKey key{std::vector<const Type *>(params.begin(), params.end()), minArgs,
                            hasRest, ret, hasThis};
  std::unique_ptr<Type> &slot = interned().functions[key];
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
    slot->minArgs_ = minArgs;
    slot->hasRest_ = hasRest;
    slot->hasThis_ = hasThis;
  }

  return slot.get();
}

const Field *Type::field(llvm::StringRef name) const {
  for (const Field &f : fields_) {
    if (f.name == name) {
      return &f;
    }
  }

  return nullptr;
}

static Interned::ObjectKey objectKey(llvm::ArrayRef<Field> fields,
                                     llvm::ArrayRef<Overload> overloads) {
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

const Overload *Type::overload(llvm::StringRef name) const {
  for (const Overload &o : overloads_) {
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
  for (const Type *t : elements) {
    if (t->isError()) {
      return t;
    }
  }
  const Type *&slot =
      interned().tuples[{std::vector<const Type *>(elements.begin(), elements.end()), readonly}];
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
    auto &type = interned().objects.emplace_back(new Type(Kind::Object, name + "]"));
    type->fields_ = std::move(fields);
    type->tuple_ = true;
    type->tupleMin_ = elements.size();
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
      interned().lists[{std::vector<const Type *>(elements.begin(), elements.end()), min, rest}];
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
  std::unique_ptr<Type> &slot = interned().results[{ok, err}];
  if (!slot) {
    std::string name = "Result<" + ok->name().str() + ", " + err->name().str() + ">";
    slot.reset(new Type(Kind::Result, std::move(name), ok));
    slot->error_ = err;
  }

  return slot.get();
}

const Type *Type::iterator(const Type *yielded, const Type *returned, const Type *sent) {
  for (const Type *t : {yielded, returned, sent}) {
    if (t->isError()) {
      return t;
    }
  }
  std::unique_ptr<Type> &slot = interned().iterators[{yielded, returned, sent}];
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
  std::unique_ptr<Type> &slot = interned().dicts[{int(kind), key, value}];
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
    slot->dictKind_ = kind;
  }

  return slot.get();
}

static std::string quotedText(const std::u16string &text) {
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

static std::string numberText(double value) {
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

static std::string utf8Text(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);

  return utf8;
}

const Type *Type::stringPattern(std::vector<std::u16string> texts,
                                std::vector<const Type *> holes) {
  if (holes.size() == 1 && holes[0]->isString() && texts[0].empty() && texts[1].empty()) {
    return holes[0];
  }
  std::unique_ptr<Type> &slot = interned().patterns[{texts, holes}];
  if (!slot) {
    std::string name = "`";
    for (size_t i = 0; i < texts.size(); ++i) {
      name += utf8Text(texts[i]);
      if (i < holes.size()) {
        name += "${" + (holes[i]->isFloat() ? std::string("number") : holes[i]->name().str()) + "}";
      }
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

const Type *Type::slotPart(UnionSlot slot) const {
  if (!isUnion()) {
    return unionSlot(this) == slot ? this : nullptr;
  }
  std::vector<const Type *> part;
  for (const Type *m : params_) {
    if (unionSlot(m) == slot) {
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
    if (base->isInt()) {
      return has(intTy()) || has(longTy()) || has(doubleTy());
    }

    return has(base);
  });
  if (has(string())) {
    llvm::erase_if(members, [](const Type *m) { return m->isPattern(); });
  }
  llvm::erase_if(members, [&](const Type *m) {
    return m->isStringLiteral() && llvm::any_of(members, [&](const Type *p) {
             return p->isPattern() && matchesPattern(m->literalText(), p);
           });
  });
  bool fractions = llvm::any_of(
      members, [](const Type *m) { return m->isLiteral() && m->literalBase()->isFloat(); });
  if (fractions) {
    for (const Type *&m : members) {
      if (m->isLiteral() && m->literalBase()->isInt()) {
        m = numberLiteral(m->literalNumber(), doubleTy());
      }
    }
  }
  if (llvm::any_of(members, [](const Type *m) { return m->isAnyObject(); })) {
    llvm::erase_if(members, [](const Type *m) { return m->isObject() && !m->isAnyObject(); });
  }
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
    std::unique_ptr<Type> &slot = interned().unions[key];
    if (!slot) {
      std::string name;
      for (const Type *m : members) {
        name += (name.empty() ? "" : " | ") +
                (m->isFunction() ? "(" + m->name().str() + ")" : m->name().str());
      }
      slot.reset(new Type(Kind::Union, std::move(name)));
      slot->params_ = members;
      for (const Type *m : members) {
        UnionSlot s = unionSlot(m);
        if (!llvm::is_contained(slot->slots_, s)) {
          slot->slots_.push_back(s);
        }
      }
      llvm::sort(slot->slots_);
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
  std::unique_ptr<Type> &slot = interned().promises[value];
  if (!slot) {
    slot.reset(new Type(Kind::Promise, "Promise<" + value->name().str() + ">", value));
  }

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

void Type::nameUnion(const Type *type, llvm::StringRef name) {
  if (type->isUnion()) {
    const_cast<Type *>(type)->name_ = name.str();
  }
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
  if (base == this || llvm::is_contained(bases_, base)) {
    return;
  }
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
  return interned().objects.emplace_back(new Type(Kind::Param, name.str())).get();
}

const Type *Type::typeOp(TypeOp op, llvm::ArrayRef<const Type *> operands) {
  const Type *&slot =
      interned().ops[{int(op), std::vector<const Type *>(operands.begin(), operands.end()), 0, 0}];
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
        name += i % 2 == 0 ? utf8Text(operands[i]->literalText()) : "${" + of(i) + "}";
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
  if (instance->isError()) {
    return instance;
  }
  std::unique_ptr<Type> &slot = interned().constructors[instance];
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
    Type *t = interned().objects.emplace_back(new Type(Kind::Object, "RegExp")).get();
    t->fields_.push_back({"lastIndex", intTy(), false});
    t->regExp_ = true;

    return t;
  }();

  return type;
}

const Type *Type::date() {
  static Type *type = [] {
    Type *t = interned().objects.emplace_back(new Type(Kind::Object, "Date")).get();
    t->fields_.push_back({"%time", doubleTy(), false});
    t->date_ = true;

    return t;
  }();

  return type;
}

const Type *Type::weakRef(const Type *target) {
  static std::map<const Type *, Type *> types;
  Type *&type = types[target];
  if (!type) {
    type =
        interned()
            .objects
            .emplace_back(new Type(Kind::Object, "WeakRef<" + target->name().str() + ">", target))
            .get();
    type->weakRef_ = true;
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
    type = interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->intl_ = kind;
  }

  return type;
}

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

const Type *Type::cryptoType(Crypto kind) {
  static const char *const names[] = {"",          "Hash", "Hmac",   "Cipheriv", "Decipheriv",
                                      "KeyObject", "Sign", "Verify", "ECDH",     "X509Certificate"};
  static Type *types[std::size(names)] = {};
  Type *&type = types[size_t(kind)];
  if (!type) {
    type = interned().objects.emplace_back(new Type(Kind::Object, names[size_t(kind)])).get();
    type->crypto_ = kind;
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
  for (const Type *t : {yielded, returned, sent}) {
    if (t->isError()) {
      return t;
    }
  }
  const Type *&slot = interned().asyncIterators[{yielded, returned, sent}];
  if (!slot) {
    std::string name = "AsyncGenerator<" + yielded->name().str();
    if (!returned->isVoid() || !sent->isVoid()) {
      name += ", " + returned->name().str();
    }
    if (!sent->isVoid()) {
      name += ", " + sent->name().str();
    }
    Type *type = interned().objects.emplace_back(new Type(Kind::Object, name + ">", yielded)).get();
    type->params_ = {returned, sent};
    type->runtime_ = Runtime::AsyncIterator;
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
  const Type *&slot = interned().matches[{groups, mandatory, named, indices}];
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
  Type *type = interned().objects.emplace_back(new Type(Kind::Object, name)).get();
  type->fields_.push_back({"%items", array(optional(string())), true});
  type->fields_.push_back({"index", intTy(), false});
  type->fields_.push_back({"input", string(), false});
  type->fields_.push_back({"groups", named ? named : optional(object({})), false});
  if (indices) {
    type->fields_.push_back({"indices", array(optional(tuple({intTy(), intTy()}))), false});
  }
  type->match_ = true;
  type->matchGroups_ = groups;
  type->mandatory_ = std::move(mandatory);
  slot = type;

  return slot;
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

}
