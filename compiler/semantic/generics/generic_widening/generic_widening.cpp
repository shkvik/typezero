#include "compiler/semantic/generics/generic_widening/generic_widening.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include <map>

namespace tz {

GenericWidening::GenericWidening(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeRelations = &all.getTypeRelations();
  this->genericClassInstantiator = &all.getGenericClassInstantiator();
}

bool GenericWidening::mentions(const TypeRef &ref, llvm::StringRef name) {
  if (ref.kind() == TypeRefKind::NAME_REF && ref.name == name) {
    return true;
  }
  for (const TypeRef &arg : ref.args) {
    if (GenericWidening::mentions(arg, name)) {
      return true;
    }
  }

  return false;
}

std::map<std::pair<const ClassDecl *, size_t>, bool> &GenericWidening::knownVariance() {
  static std::map<std::pair<const ClassDecl *, size_t>, bool> known;

  return known;
}

std::string GenericWidening::writerOf(const ClassDecl *generic, llvm::StringRef name) {
  for (const FunctionDecl *fn : generic->methods) {
    if (fn->isStatic() || fn->access() != Access::Public) {
      continue;
    }
    for (const Param &param : fn->params) {
      if (param.type && GenericWidening::mentions(*param.type, name)) {
        return ErrorTextVariance::takes(fn->name, name);
      }
    }
  }
  for (const ClassField &field : generic->fields) {
    if (field.isStatic() || field.access != Access::Public || field.readonly) {
      continue;
    }
    if (field.type && GenericWidening::mentions(*field.type, name)) {
      return ErrorTextVariance::settable(field.name);
    }
  }

  return "";
}

bool GenericWidening::writtenFromOutside(const ClassDecl *generic, llvm::StringRef name) {
  for (const FunctionDecl *fn : generic->methods) {
    if (fn->isStatic() || fn->access() != Access::Public) {
      continue;
    }
    for (const Param &param : fn->params) {
      if (param.type && GenericWidening::mentions(*param.type, name)) {
        return true;
      }
    }
  }
  for (const ClassField &field : generic->fields) {
    if (field.isStatic() || field.access != Access::Public || field.readonly) {
      continue;
    }
    if (field.type && GenericWidening::mentions(*field.type, name)) {
      return true;
    }
  }
  for (const FunctionDecl *constructor : generic->constructors) {
    for (const Param &param : constructor->params) {
      bool open =
          param.property && param.property->access == Access::Public && !param.property->readonly;
      if (open && param.type && GenericWidening::mentions(*param.type, name)) {
        return true;
      }
    }
  }

  return false;
}

bool GenericWidening::passedOnSafely(const ClassDecl *generic, llvm::StringRef name) {
  if (!generic->base || !GenericWidening::mentions(*generic->base, name)) {
    return true;
  }
  const ClassDecl *parent = generic->parent;
  const ClassDecl *baseGeneric = parent && parent->generic() ? parent->generic() : parent;
  if (!baseGeneric) {
    return false;
  }
  const std::vector<TypeRef> &args = generic->base->args;
  for (size_t i = 0; i < args.size(); ++i) {
    if (!GenericWidening::mentions(args[i], name)) {
      continue;
    }
    bool direct = args[i].kind() == TypeRefKind::NAME_REF && args[i].name == name;
    if (!direct || !GenericWidening::covariant(baseGeneric, i)) {
      return false;
    }
  }

  return true;
}

bool GenericWidening::referenceFits(const Type *own, const Type *wide) {
  if (own == wide || own->isNever()) {
    return true;
  }
  if (own->isUnion() && !own->isTagged()) {
    return llvm::all_of(own->members(),
                        [&](const Type *m) { return GenericWidening::referenceFits(m, wide); });
  }
  if (wide->isUnion() && !wide->isTagged()) {
    return llvm::any_of(wide->members(),
                        [&](const Type *m) { return GenericWidening::referenceFits(own, m); });
  }

  return WideningRelations::sameReference(own, wide);
}

bool GenericWidening::covariant(const ClassDecl *generic, size_t index) {
  if (index >= generic->typeParams.size()) {
    return false;
  }
  auto key = std::make_pair(generic, index);
  std::map<std::pair<const ClassDecl *, size_t>, bool> &known = GenericWidening::knownVariance();
  if (auto found = known.find(key); found != known.end()) {
    return found->second;
  }
  known[key] = false;
  llvm::StringRef name = generic->typeParams[index].name;
  bool safe = !GenericWidening::writtenFromOutside(generic, name) &&
              GenericWidening::passedOnSafely(generic, name);
  known[key] = safe;

  return safe;
}

const ClassDecl *GenericWidening::instanceOf(const ClassDecl *cls, const ClassDecl *generic) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (c->generic() == generic) {
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
  if (!target || !source || !target->generic()) {
    return false;
  }
  const ClassDecl *instance = GenericWidening::instanceOf(source, target->generic());
  if (!instance || instance == target || instance->typeArgs.size() != target->typeArgs.size()) {
    return false;
  }
  for (size_t i = 0; i < target->typeArgs.size(); ++i) {
    const Type *own = instance->typeArgs[i];
    const Type *wide = target->typeArgs[i];
    if (own == wide) {
      continue;
    }
    if (!GenericWidening::covariant(target->generic(), i) ||
        !GenericWidening::referenceFits(own, wide)) {
      return false;
    }
  }

  return true;
}

std::string GenericWidening::whyNot(const Type *from, const Type *to) {
  if (!from->isClass() || !to->isClass() || !from->classDecl() || !to->classDecl() ||
      !to->classDecl()->generic()) {
    return "";
  }
  const ClassDecl *target = to->classDecl();
  const ClassDecl *instance = GenericWidening::instanceOf(from->classDecl(), target->generic());
  if (!instance || instance->typeArgs.size() != target->typeArgs.size()) {
    return "";
  }
  for (size_t i = 0; i < target->typeArgs.size(); ++i) {
    const Type *own = instance->typeArgs[i];
    const Type *wide = target->typeArgs[i];
    if (own == wide) {
      continue;
    }
    if (GenericWidening::covariant(target->generic(), i)) {
      if (GenericWidening::referenceFits(own, wide)) {
        continue;
      }
      std::string ownName = own->name().str();
      std::string wideName = wide->name().str();
      bool named = !ownName.empty() && ownName.front() != '{' && !own->isUnion();
      std::string example = named ? ownName : ErrorTextVariance::sampleName();

      return ErrorTextVariance::noExtends(ownName, wideName, example);
    }
    llvm::StringRef name = target->generic()->typeParams[i].name;
    std::string writer = GenericWidening::writerOf(target->generic(), name);
    if (writer.empty()) {
      writer = ErrorTextVariance::baseTakes(name);
    }

    return ErrorTextVariance::wider(target->generic()->name, name, writer);
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
    if (!candidate->generic()) {
      continue;
    }
    ClassDecl *generic = candidate->generic();
    std::vector<const ClassDecl *> instances;
    for (const Type *member : members) {
      const ClassDecl *cls = member->isClass() ? member->classDecl() : nullptr;
      const ClassDecl *instance = cls ? GenericWidening::instanceOf(cls, generic) : nullptr;
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
      joinable = GenericWidening::covariant(generic, i);
      args.push_back(Type::unionOf(seen));
    }
    if (!joinable) {
      return expr;
    }
    GenericClassInstantiator::ResultClassMade widened =
        this->genericClassInstantiator->instantiateClass(generic, args, expr->loc());
    if (widened.isErr() &&
        widened.error().code !=
            GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
      llvm::SMLoc at = widened.error().code ==
                               GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
                           ? generic->loc()
                           : expr->loc();
      this->state.getDiagnostics().error(at, widened.error().message);
    }
    if (widened.isErr() || !TypeRelations::isImplicitlyConvertible(type, widened.value()->type)) {
      return expr;
    }

    return this->typeRelations->convert(expr, widened.value()->type);
  }

  return expr;
}

}
