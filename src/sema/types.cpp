#include "sema/types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorHandling.h"

#include <map>
#include <memory>
#include <tuple>

namespace tz {

bool Type::isManaged() const {
  switch (kind_) {
  case Kind::String:
  case Kind::Array:
  case Kind::Function: // its environment
  case Kind::Object:
    return true;
  case Kind::Optional:
    return inner_->isManaged();
  default:
    return false;
  }
}

bool Type::hasZeroValue() const {
  return isNumeric() || isBool() || isString() || isOptional();
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
const Type *Type::string() { static const Type t(Kind::String, "string"); return &t; }

namespace {

// Composite types, created on first use and kept for the whole compilation.
struct Interned {
  llvm::DenseMap<const Type *, std::unique_ptr<Type>> optionals, arrays;
  using FunctionKey = std::tuple<std::vector<const Type *>, size_t, bool, const Type *>;
  std::map<FunctionKey, std::unique_ptr<Type>> functions;
  // Object types by their fields, sorted by name; the recursive ones apart.
  using ObjectKey = std::vector<std::tuple<std::string, const Type *, bool>>;
  std::map<ObjectKey, const Type *> objectsByKey;
  std::vector<std::unique_ptr<Type>> objects;
};

Interned &interned() {
  static Interned instance;
  return instance;
}

} // namespace

const Type *Type::optional(const Type *type) {
  if (type->isError() || type->isUndefined() || type->isOptional())
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
    std::string name = element->isOptional() || element->isFunction()
                           ? "(" + element->name().str() + ")[]"
                           : element->name().str() + "[]";
    slot.reset(new Type(Kind::Array, std::move(name), element));
  }
  return slot.get();
}

// `(int, string?) => double`: a parameter that may be left out shows its value
// type with `?`.
const Type *Type::function(llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                           const Type *ret) {
  for (const Type *t : params)
    if (t->isError())
      return t;
  if (ret->isError())
    return ret;
  Interned::FunctionKey key{std::vector<const Type *>(params.begin(), params.end()), minArgs, hasRest, ret};
  std::unique_ptr<Type> &slot = interned().functions[key];
  if (!slot) {
    std::string name = "(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0)
        name += ", ";
      if (hasRest && i + 1 == params.size())
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
  }
  return slot.get();
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

static Interned::ObjectKey objectKey(llvm::ArrayRef<Field> fields) {
  Interned::ObjectKey key;
  for (const Field &f : fields)
    key.emplace_back(f.name, f.type, f.readonly);
  llvm::sort(key);
  return key;
}

const Type *Type::object(std::vector<Field> fields, llvm::StringRef name) {
  for (const Field &f : fields)
    if (f.type->isError())
      return f.type;
  const Type *&slot = interned().objectsByKey[objectKey(fields)];
  if (!slot) {
    std::string spelling = name.empty() ? objectTypeSpelling(fields) : name.str();
    auto &type = interned().objects.emplace_back(new Type(Kind::Object, std::move(spelling)));
    type->fields_ = std::move(fields);
    slot = type.get();
  }
  return slot;
}

Type *Type::declareObject(llvm::StringRef name) {
  auto &type = interned().objects.emplace_back(new Type(Kind::Object, name.str()));
  type->incomplete_ = true;
  return type.get();
}

void Type::defineFields(std::vector<Field> fields) {
  fields_ = std::move(fields);
  incomplete_ = false;
}

const char *typeOfName(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Int:
  case Type::Kind::Long:
  case Type::Kind::Double: return "number";
  case Type::Kind::String: return "string";
  case Type::Kind::Bool: return "boolean";
  case Type::Kind::Array:
  case Type::Kind::Object: return "object";
  case Type::Kind::Function: return "function";
  case Type::Kind::Optional: return typeOfName(type->inner());
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
      .Case("string", string())
      .Default(nullptr);
}

} // namespace tz
