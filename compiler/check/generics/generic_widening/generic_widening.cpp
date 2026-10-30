#include "compiler/check/generics/generic_widening/generic_widening.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
#include <map>

namespace tz {

namespace {

bool mentions(const TypeRef &ref, llvm::StringRef name) {
  if (ref.kind == TypeRef::Kind::Name && ref.name == name) {
    return true;
  }
  for (const TypeRef &arg : ref.args) {
    if (mentions(arg, name)) {
      return true;
    }
  }

  return false;
}

std::map<std::pair<const ClassDecl *, size_t>, bool> &knownVariance() {
  static std::map<std::pair<const ClassDecl *, size_t>, bool> known;

  return known;
}

std::string writerOf(const ClassDecl *generic, llvm::StringRef name) {
  for (const FunctionDecl *fn : generic->methods) {
    if (fn->isStatic || fn->access != Access::Public) {
      continue;
    }
    for (const Param &param : fn->params) {
      if (param.type && mentions(*param.type, name)) {
        return "its public method '" + fn->name.str() + "' takes " + name.str();
      }
    }
  }
  for (const ClassField &field : generic->fields) {
    if (field.isStatic || field.access != Access::Public || field.readonly) {
      continue;
    }
    if (field.type && mentions(*field.type, name)) {
      return "its public field '" + field.name.str() + "' can be set";
    }
  }

  return "";
}

bool writtenFromOutside(const ClassDecl *generic, llvm::StringRef name) {
  for (const FunctionDecl *fn : generic->methods) {
    if (fn->isStatic || fn->access != Access::Public) {
      continue;
    }
    for (const Param &param : fn->params) {
      if (param.type && mentions(*param.type, name)) {
        return true;
      }
    }
  }
  for (const ClassField &field : generic->fields) {
    if (field.isStatic || field.access != Access::Public || field.readonly) {
      continue;
    }
    if (field.type && mentions(*field.type, name)) {
      return true;
    }
  }
  for (const FunctionDecl *constructor : generic->constructors) {
    for (const Param &param : constructor->params) {
      bool open = param.property && param.access == Access::Public && !param.readonly;
      if (open && param.type && mentions(*param.type, name)) {
        return true;
      }
    }
  }

  return false;
}

bool passedOnSafely(const ClassDecl *generic, llvm::StringRef name) {
  if (!generic->base || !mentions(*generic->base, name)) {
    return true;
  }
  const ClassDecl *parent = generic->parent;
  const ClassDecl *baseGeneric = parent && parent->generic ? parent->generic : parent;
  if (!baseGeneric) {
    return false;
  }
  const std::vector<TypeRef> &args = generic->base->args;
  for (size_t i = 0; i < args.size(); ++i) {
    if (!mentions(args[i], name)) {
      continue;
    }
    bool direct = args[i].kind == TypeRef::Kind::Name && args[i].name == name;
    if (!direct || !GenericWidening::covariant(baseGeneric, i)) {
      return false;
    }
  }

  return true;
}

bool referenceFits(const Type *own, const Type *wide) {
  if (own == wide || own->isNever()) {
    return true;
  }
  if (own->isUnion() && !own->isTagged()) {
    return llvm::all_of(own->members(), [&](const Type *m) { return referenceFits(m, wide); });
  }
  if (wide->isUnion() && !wide->isTagged()) {
    return llvm::any_of(wide->members(), [&](const Type *m) { return referenceFits(own, m); });
  }

  return WideWrites::sameReference(own, wide);
}

}

GenericWidening::GenericWidening(Checker &checker) : checker_(checker) {}

bool GenericWidening::covariant(const ClassDecl *generic, size_t index) {
  if (index >= generic->typeParams.size()) {
    return false;
  }
  auto key = std::make_pair(generic, index);
  std::map<std::pair<const ClassDecl *, size_t>, bool> &known = knownVariance();
  if (auto found = known.find(key); found != known.end()) {
    return found->second;
  }
  known[key] = false;
  llvm::StringRef name = generic->typeParams[index].name;
  bool safe = !writtenFromOutside(generic, name) && passedOnSafely(generic, name);
  known[key] = safe;

  return safe;
}

const ClassDecl *GenericWidening::instanceOf(const ClassDecl *cls, const ClassDecl *generic) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (c->generic == generic) {
      return c;
    }
  }

  return nullptr;
}

bool GenericWidening::widens(const Type *from, const Type *to) {
  if (!from->isClass() || !to->isClass()) {
    return false;
  }
  const ClassDecl *target = to->classDecl();
  const ClassDecl *source = from->classDecl();
  if (!target || !source || !target->generic) {
    return false;
  }
  const ClassDecl *instance = instanceOf(source, target->generic);
  if (!instance || instance == target || instance->typeArgs.size() != target->typeArgs.size()) {
    return false;
  }
  for (size_t i = 0; i < target->typeArgs.size(); ++i) {
    const Type *own = instance->typeArgs[i], *wide = target->typeArgs[i];
    if (own == wide) {
      continue;
    }
    if (!covariant(target->generic, i) || !referenceFits(own, wide)) {
      return false;
    }
  }

  return true;
}

std::string GenericWidening::whyNot(const Type *from, const Type *to) {
  if (!from->isClass() || !to->isClass() || !from->classDecl() || !to->classDecl() ||
      !to->classDecl()->generic) {
    return "";
  }
  const ClassDecl *target = to->classDecl();
  const ClassDecl *instance = instanceOf(from->classDecl(), target->generic);
  if (!instance || instance->typeArgs.size() != target->typeArgs.size()) {
    return "";
  }
  for (size_t i = 0; i < target->typeArgs.size(); ++i) {
    const Type *own = instance->typeArgs[i], *wide = target->typeArgs[i];
    if (own == wide) {
      continue;
    }
    if (covariant(target->generic, i)) {
      if (referenceFits(own, wide)) {
        continue;
      }
      std::string ownName = own->name().str(), wideName = wide->name().str();
      bool named = !ownName.empty() && ownName.front() != '{' && !own->isUnion();
      std::string example = named ? ownName : "MyError";

      return "; '" + ownName + "' does not extend '" + wideName +
             "': a type goes where a wider one is wanted only when it says so, e.g. "
             "'interface " +
             example + " extends " + wideName + " { ... }'";
    }
    llvm::StringRef name = target->generic->typeParams[i].name;
    std::string writer = writerOf(target->generic, name);
    if (writer.empty()) {
      writer = "its base class takes " + name.str() + " in";
    }

    return "; '" + target->generic->name.str() + "' with a wider " + name.str() +
           " would let a value of another type into it: " + writer;
  }

  return "";
}

Expr *GenericWidening::joined(Expr *expr) {
  const Type *type = expr->type;
  if (!type->isUnion()) {
    return expr;
  }
  llvm::ArrayRef<const Type *> members = type->members();
  const ClassDecl *first = members.front()->classDecl();
  if (!members.front()->isClass() || !first) {
    return expr;
  }
  for (const ClassDecl *candidate = first; candidate; candidate = candidate->parent) {
    if (!candidate->generic) {
      continue;
    }
    ClassDecl *generic = candidate->generic;
    std::vector<const ClassDecl *> instances;
    for (const Type *member : members) {
      const ClassDecl *cls = member->isClass() ? member->classDecl() : nullptr;
      const ClassDecl *instance = cls ? instanceOf(cls, generic) : nullptr;
      if (!instance || instance->typeArgs.size() != generic->typeParams.size()) {
        break;
      }
      instances.push_back(instance);
    }
    if (instances.size() != members.size()) {
      continue;
    }
    std::vector<const Type *> args;
    bool joinable = true;
    for (size_t i = 0; i < generic->typeParams.size() && joinable; ++i) {
      std::vector<const Type *> seen;
      for (const ClassDecl *instance : instances) {
        if (!llvm::is_contained(seen, instance->typeArgs[i])) {
          seen.push_back(instance->typeArgs[i]);
        }
      }
      if (seen.size() == 1) {
        args.push_back(seen.front());
        continue;
      }
      joinable = covariant(generic, i);
      args.push_back(Type::unionOf(seen));
    }
    if (!joinable) {
      return expr;
    }
    ClassDecl *made =
        checker_.genericClassInstantiator().instantiateClass(generic, args, expr->loc());
    if (!made || !TypeRelations::isImplicitlyConvertible(type, made->type)) {
      return expr;
    }

    return checker_.typeRelations().convert(expr, made->type);
  }

  return expr;
}

}
