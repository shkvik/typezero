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

std::string objectTypeSpelling(llvm::ArrayRef<Field> fields) {
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

namespace {

const char *integerName(const Type *type) {
  uint64_t max = type->intMax();
  if (type->isUnsigned()) {
    return max == 255 ? "uint8" : max == 65535 ? "uint16" : max == 4294967295u ? "uint32" : "uint64";
  }

  return max == 127 ? "int8" : max == 32767 ? "int16" : max == 2147483647 ? "int32" : "int";
}

}

bool isTypeOfName(llvm::StringRef name) { return llvm::is_contained(kTypeOfNames, name); }

std::vector<llvm::StringRef> typeOfNames(const Type *type) {
  std::vector<llvm::StringRef> names;
  auto add = [&](llvm::StringRef name) {
    if (!name.empty() && !llvm::is_contained(names, name)) {
      names.push_back(name);
    }
  };
  if (type->isOptional()) {
    for (llvm::StringRef name : typeOfNames(type->inner())) {
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
      for (llvm::StringRef name : typeOfNames(m)) {
        add(name);
      }
    }
    return names;
  }
  add(typeOfName(type));

  return names;
}

const char *typeOfName(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Int:
    return integerName(type);
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
    return typeOfName(type->inner());
  case Type::Kind::Param:
    return "";
  case Type::Kind::Json:
    return "";
  case Type::Kind::Literal:
    return typeOfName(type->literalBase());
  case Type::Kind::Union: {
    const char *name = typeOfName(type->members().front());
    for (const Type *m : type->members()) {
      if (llvm::StringRef(typeOfName(m)) != name) {
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

std::string jsonTagField(llvm::ArrayRef<const Type *> objects) {
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
