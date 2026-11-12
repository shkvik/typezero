#include "compiler/program/types/type_spelling/type_spelling.h"
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

const char *TypeSpelling::integerName(const Type *type) {
  uint64_t max = type->intMax();
  if (type->isUnsigned()) {
    return max == 255           ? "uint8"
           : max == 65535       ? "uint16"
           : max == 4294967295u ? "uint32"
                                : "uint64";
  }

  return max == 127 ? "int8" : max == 32767 ? "int16" : max == 2147483647 ? "int32" : "int";
}

std::string TypeSpelling::objectTypeSpelling(llvm::ArrayRef<Field> fields) {
  if (fields.empty()) {
    return "{}";
  }
  std::string text = "{ ";
  for (size_t i = 0; i < fields.size(); ++i) {
    const Field &f = fields[i];
    if (i > 0) {
      text += "; ";
    }
    if (f.readonly) {
      text += "readonly ";
    }
    text += f.name;
    if (f.type->isOptional()) {
      text += "?: " + f.type->inner()->name().str();
    } else {
      text += ": " + f.type->name().str();
    }
  }

  return text + " }";
}

bool TypeSpelling::isTypeOfName(llvm::StringRef name) {
  return llvm::is_contained(kTypeOfNames, name);
}

bool TypeSpelling::isNumberTypeOfName(llvm::StringRef name) {
  return name.starts_with("int") || name.starts_with("uint") || name == "double" ||
         name == "decimal";
}

const Type *TypeSpelling::narrowedByTypeOf(const std::u16string &name) {
  static const TypeOfNarrowingInfo narrowings[] = {
      {u"string", TypeOfNarrowing::STRING},
      {u"double", TypeOfNarrowing::DOUBLE},
      {u"boolean", TypeOfNarrowing::BOOLEAN},
      {u"object", TypeOfNarrowing::ANY_OBJECT},
      {u"class", TypeOfNarrowing::ANY_OBJECT},
      {u"decimal", TypeOfNarrowing::DECIMAL},
      {u"int", TypeOfNarrowing::INT},
      {u"int8", TypeOfNarrowing::INT8},
      {u"int16", TypeOfNarrowing::INT16},
      {u"int32", TypeOfNarrowing::INT32},
      {u"uint8", TypeOfNarrowing::UINT8},
      {u"uint16", TypeOfNarrowing::UINT16},
      {u"uint32", TypeOfNarrowing::UINT32},
      {u"uint64", TypeOfNarrowing::UINT64},
      {u"array", TypeOfNarrowing::UNKNOWN_ARRAY},
  };
  for (const TypeOfNarrowingInfo &info : narrowings) {
    if (name == info.name) {
      return TypeSpelling::narrowedType(info.narrowing);
    }
  }

  return nullptr;
}

const Type *TypeSpelling::narrowedType(TypeOfNarrowing narrowing) {
  switch (narrowing) {
    case TypeOfNarrowing::STRING:
      return Type::string();
    case TypeOfNarrowing::DOUBLE:
      return Type::doubleTy();
    case TypeOfNarrowing::BOOLEAN:
      return Type::boolTy();
    case TypeOfNarrowing::ANY_OBJECT:
      return Type::anyObject();
    case TypeOfNarrowing::DECIMAL:
      return Type::decimalTy();
    case TypeOfNarrowing::INT:
      return Type::intTy();
    case TypeOfNarrowing::INT8:
      return Type::signedInteger(8);
    case TypeOfNarrowing::INT16:
      return Type::signedInteger(16);
    case TypeOfNarrowing::INT32:
      return Type::signedInteger(32);
    case TypeOfNarrowing::UINT8:
      return Type::unsignedInteger(8);
    case TypeOfNarrowing::UINT16:
      return Type::unsignedInteger(16);
    case TypeOfNarrowing::UINT32:
      return Type::unsignedInteger(32);
    case TypeOfNarrowing::UINT64:
      return Type::unsignedInteger(64);
    case TypeOfNarrowing::UNKNOWN_ARRAY:
      return Type::array(Type::unknown());
  }

  return nullptr;
}

std::vector<llvm::StringRef> TypeSpelling::typeOfNames(const Type *type) {
  std::vector<llvm::StringRef> names;
  auto add = [&](llvm::StringRef name) {
    if (!name.empty() && !llvm::is_contained(names, name)) {
      names.push_back(name);
    }
  };
  if (type->isOptional()) {
    for (llvm::StringRef name : TypeSpelling::typeOfNames(type->inner())) {
      add(name);
    }
    add("undefined");
    return names;
  }
  if (type->isJson()) {
    for (const char *name : {"null", "boolean", "double", "string", "array", "object"}) {
      add(name);
    }
    return names;
  }
  if (type->isUnknown() || type->isParam()) {
    for (const char *name : kTypeOfNames) {
      if (llvm::StringRef(name) != "null" || type->isParam()) {
        add(name);
      }
    }
    return names;
  }
  if (type->isAnyObject()) {
    add("object");
    add("class");
    return names;
  }
  if (type->isUnion()) {
    for (const Type *m : type->members()) {
      for (llvm::StringRef name : TypeSpelling::typeOfNames(m)) {
        add(name);
      }
    }
    return names;
  }
  add(TypeSpelling::typeOfName(type));

  return names;
}

const char *TypeSpelling::typeOfName(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Int:
    return TypeSpelling::integerName(type);
  case Type::Kind::Double:
    return "double";
  case Type::Kind::Decimal:
    return "decimal";
  case Type::Kind::String:
    return "string";
  case Type::Kind::Bool:
    return "boolean";
  case Type::Kind::Array:
    return "array";
  case Type::Kind::Object:
    if (type->isAnyObject()) {
      return "";
    }
    return type->isTuple() ? "array" : "object";
  case Type::Kind::Result:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
    return "object";
  case Type::Kind::Function:
    return "function";
  case Type::Kind::Optional:
    return TypeSpelling::typeOfName(type->inner());
  case Type::Kind::Param:
    return "";
  case Type::Kind::Json:
    return "";
  case Type::Kind::Literal:
    return TypeSpelling::typeOfName(type->literalBase());
  case Type::Kind::Union: {
    const char *name = TypeSpelling::typeOfName(type->members().front());
    for (const Type *m : type->members()) {
      if (llvm::StringRef(TypeSpelling::typeOfName(m)) != name) {
        return "";
      }
    }
    return name;
  }
  case Type::Kind::Unknown:
    return "";
  case Type::Kind::ClassRef:
    return "class";
  default:
    return "undefined";
  }
}

std::string TypeSpelling::jsonTagField(llvm::ArrayRef<const Type *> objects) {
  if (objects.empty() || !objects.front()->isObject()) {
    return "";
  }
  for (const Field &f : objects.front()->fields()) {
    std::vector<const Type *> tags;
    for (const Type *m : objects) {
      const Field *g = m->isObject() ? m->field(f.name) : nullptr;
      if (!g || !g->type->isLiteral() || llvm::is_contained(tags, g->type)) {
        break;
      }
      tags.push_back(g->type);
    }
    if (tags.size() == objects.size()) {
      return f.name;
    }
  }

  return "";
}

}
