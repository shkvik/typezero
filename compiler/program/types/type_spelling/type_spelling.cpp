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

const char *typeOfName(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Int:
  case Type::Kind::Long:
  case Type::Kind::Double:
    return "number";
  case Type::Kind::String:
    return "string";
  case Type::Kind::Bool:
    return "boolean";
  case Type::Kind::Array:
  case Type::Kind::Object:
  case Type::Kind::Result:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
  case Type::Kind::Promise:
  case Type::Kind::Decimal:
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
    return "function";
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
