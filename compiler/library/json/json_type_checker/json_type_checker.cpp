#include "compiler/library/json/json_type_checker/json_type_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

using ClassProblem = llvm::function_ref<std::string(const ClassDecl *)>;

std::string fromJsonProblem(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen,
                            ClassProblem classProblem = nullptr) {
  switch (type->kind()) {
  case Type::Kind::Int:
  case Type::Kind::Double:
  case Type::Kind::Decimal:
  case Type::Kind::Bool:
  case Type::Kind::String:
  case Type::Kind::Json:
    return "";
  case Type::Kind::Optional:
  case Type::Kind::Array:
    return fromJsonProblem(type->inner(), seen, classProblem);
  case Type::Kind::Object: {
    if (const ClassDecl *cls = type->classDecl(); cls && !cls->literal && classProblem) {
      return seen.insert(type).second ? classProblem(cls) : "";
    }
    if (type->isClass()) {
      return "'" + type->name().str() +
             "' is a class, and only its constructor makes one; make it with 'fromJson(" +
             type->name().str() +
             ", j)', which calls the constructor with the keys of the Json "
             "named as its parameters (docs/decorators.md, section 6)";
    }
    if (type->isTuple()) {
      return "tuples are not supported yet; read an array, or an object with named fields";
    }
    if (type->isRegExp()) {
      return "a RegExp comes only from a literal or 'new RegExp'; read the pattern as a string "
             "and pass it to 'new RegExp(pattern)'";
    }
    if (type->isMatch()) {
      return "a match comes only from a regex; read the text and match it";
    }
    if (type->isDate()) {
      return "JSON has no dates, and JSON.parse in JavaScript gives the string JSON.stringify "
             "wrote; read it as a 'string' and make the date with 'Date.parse(text)'";
    }
    if (type->isIntl()) {
      return type->intlKind() == IntlKind::Segments
                 ? "Intl.Segments come only from 'segment(text)' of an Intl.Segmenter"
                 : "an " + type->name().str() + " comes only from 'new " + type->name().str() +
                       "(...)'";
    }
    if (type->isCrypto()) {
      return "a " + type->name().str() + " comes only from the functions of 'node:crypto'";
    }
    if (type->isBytes()) {
      return "JSON has no binary data; read the bytes as a 'Json' or an 'int[]' and make the '" +
             type->name().str() + "' from them";
    }
    if (type->isAnyObject() || type->isIncomplete()) {
      return "the type is not known here";
    }
    if (!seen.insert(type).second) {
      return "";
    }
    for (const Field &f : type->fields()) {
      if (std::string problem = fromJsonProblem(f.type, seen, classProblem); !problem.empty()) {
        return problem;
      }
    }
    return "";
  }
  case Type::Kind::Dict:
    if (type->isRecord()) {
      return fromJsonProblem(type->valueType(), seen, classProblem);
    }
    return "JSON has objects, not a " + std::string(type->isMap() ? "Map" : "Set") + "; read a " +
           (type->isMap() ? "'Record<string, T>'" : "'T[]'") + " and make the " +
           (type->isMap() ? "Map of it, 'new Map(Object.entries(r))'" : "Set of it, 'new Set(xs)'");
  case Type::Kind::Param:
    return "a type parameter is not known until the function is called; give the type itself";
  case Type::Kind::Literal:
    return "";
  case Type::Kind::Union: {
    std::vector<const Type *> numbers, objects;
    for (const Type *m : type->members()) {
      if (std::string problem = fromJsonProblem(m, seen); !problem.empty()) {
        return problem;
      }
      const Type *base = m->isLiteral() ? m->literalBase() : m;
      if ((base->isNumeric() || base->isDecimal()) && !m->isLiteral()) {
        numbers.push_back(m);
      }
      if (m->isObject() || m->isDict()) {
        objects.push_back(m);
      }
    }
    if (numbers.size() > 1) {
      return "'" + numbers[0]->name().str() + "' and '" + numbers[1]->name().str() +
             "' are both numbers in JSON, so a number could be either; keep one of them";
    }
    if (objects.size() > 1) {
      auto record = llvm::find_if(objects, [](const Type *m) { return m->isDict(); });
      if (record != objects.end()) {
        return "a Record is an object in JSON, as '" +
               (*record == objects[0] ? objects[1] : objects[0])->name().str() +
               "' is, so an object could be either; keep one of them";
      }
      if (jsonTagField(objects).empty()) {
        return "'" + objects[0]->name().str() + "' and '" + objects[1]->name().str() +
               "' are both objects in JSON, and no field tells them apart; give each a field "
               "of a literal type with its own value, e.g. 'kind: \"circle\"'";
      }
    }
    return "";
  }
  default:
    return "a value of type '" + type->name().str() + "' cannot come from JSON";
  }
}

}

JsonTypeChecker::JsonTypeChecker(Checker &checker) : checker_(checker) {}

std::string JsonTypeChecker::jsonProblem(const Type *type, Checker::ClassProblem classProblem) {
  llvm::SmallPtrSet<const Type *, 8> seen;

  return fromJsonProblem(type, seen, classProblem);
}

bool JsonTypeChecker::checkFromJsonType(const Type *type, llvm::SMLoc loc, llvm::StringRef fn) {
  if (type->isError()) {
    return false;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  std::string problem = fromJsonProblem(type, seen);
  if (problem.empty()) {
    return true;
  }
  checker_.diags_.error(loc, fn + "<" + type->name() + "> cannot build a '" + type->name() +
                                 "': " + problem);

  return false;
}

}
