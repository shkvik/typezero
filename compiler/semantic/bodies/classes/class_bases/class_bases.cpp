#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/library/events/event_map_inference/event_map_inference.h"
#include "compiler/semantic/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::isa;

namespace tz {

ClassBases::ClassBases(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fsDeclarer = &all.getFsDeclarer();
  this->typeDeclarer = &all.getTypeDeclarer();
  this->typeResolver = &all.getTypeResolver();
  this->classDeclarer = &all.getClassDeclarer();
  this->moduleDeclarer = &all.getModuleDeclarer();
  this->eventMapInference = &all.getEventMapInference();
  this->genericClassInstantiator = &all.getGenericClassInstantiator();
}

bool ClassBases::isErrorClass(llvm::StringRef name) {
  static const char *const names[] = {"Error",       "TypeError",      "RangeError",
                                      "SyntaxError", "ReferenceError", "EvalError",
                                      "URIError",    "AggregateError"};

  return llvm::is_contained(names, name);
}

const char *ClassBases::accessName(Access access) {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
}

void ClassBases::addBase(const Type *type, const Type *base) {
  const_cast<Type *>(type)->addBase(base);
  bool emitted =
      !this->state.getFunctionStack().isInsideTrial() &&
      llvm::none_of(this->state.getFunctionStack().getFunctions(), [](const FunctionDecl *fn) {
        return !fn->typeParams.empty() && !fn->isInstance;
      });
  if (emitted) {
    this->state.getInventoryStore().addSubtype(type);
  }
}

ClassBases::ResultParentClassFound ClassBases::baseClass(const ClassDecl *cls, const TypeRef &ref,
                                                         bool named, bool instance,
                                                         ClassDecl *parent,
                                                         const Stmt *found) const {
  if (!named) {
    std::string message = ErrorTextExtends::byName();

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::NOT_NAMED,
        .message = message,
    });
  }
  if (instance && !parent) {
    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (parent && (parent == cls || parent->stage == ClassStage::RESOLVING_BASE)) {
    llvm::StringRef through = parent == cls ? llvm::StringRef() : parent->name;
    std::string message = ErrorTextExtends::itself(cls->name, through);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::EXTENDS_ITSELF,
        .message = message,
    });
  }
  if (this->fsDeclarer->isRuntimeClass(parent)) {
    std::string message = ErrorTextExtends::runtime(cls->name, parent->name);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (parent && parent->baseExpr && parent->stage == ClassStage::UNRESOLVED) {
    std::string message = ErrorTextExtends::valueBase(cls->name, parent->name);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::VALUE_BASE,
        .message = message,
    });
  }
  if (parent) {
    return ok(parent);
  }
  if (!found && ClassBases::isErrorClass(ref.name)) {
    std::string message = ErrorTextExtends::errorClass(ref.name, cls->name);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::ERROR_CLASS,
        .message = message,
    });
  }
  if (found) {
    bool alias = isa<TypeAliasDecl>(found);
    std::string message = ErrorTextExtends::notClass(ref.name, cls->name, alias);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }
  std::string message = ErrorTextExtends::noClass(ref.name);

  return err(ClassBasesBaseClassError{
      .code = ClassBasesBaseClassErrorCode::NO_SUCH_CLASS,
      .message = message,
  });
}

ClassBases::ResultImplementedShape ClassBases::implementedType(const Type *type,
                                                               const ClassDecl *cls) {
  if (type->isClass()) {
    std::string message = ErrorTextExtends::classIface(type->name(), static_cast<bool>(cls->base));

    return err(ClassBasesImplementedTypeError{
        .code = ClassBasesImplementedTypeErrorCode::CLASS_AS_INTERFACE,
        .message = message,
    });
  }
  bool shape =
      type->isObject() && !type->isTuple() && !type->isAnyObject() && !type->isIncomplete();
  if (!shape) {
    std::string message = ErrorTextExtends::notIface(type->name());

    return err(ClassBasesImplementedTypeError{
        .code = ClassBasesImplementedTypeErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(type);
}

ClassBases::ResultOverrideTargeted ClassBases::overrideTarget(const ClassDecl *cls,
                                                              const ClassField &field) {
  if (field.isOverride && cls->parent) {
    std::string message = ErrorTextRedeclared::noField(field.name, cls->parent->name);

    return err(ClassBasesOverrideTargetError{
        .code = ClassBasesOverrideTargetErrorCode::NO_FIELD,
        .message = message,
    });
  }
  if (field.isOverride) {
    std::string message = ErrorTextRedeclared::noParent(field.name, cls->name);

    return err(ClassBasesOverrideTargetError{
        .code = ClassBasesOverrideTargetErrorCode::NO_PARENT,
        .message = message,
    });
  }

  return ok(&field);
}

ClassBases::ResultInheritedVisible ClassBases::inheritedVisible(const ClassField &field,
                                                                const ClassField *inherited,
                                                                const ClassDecl *owner) {
  if (inherited->access == Access::Private) {
    std::string message = ErrorTextRedeclared::isPrivate(field.name, owner->name);

    return err(ClassBasesInheritedVisibleError{
        .code = ClassBasesInheritedVisibleErrorCode::PRIVATE_IN_BASE,
        .message = message,
    });
  }

  return ok(inherited);
}

ClassBases::ResultAccessIsNotLower ClassBases::accessKept(const ClassField &field,
                                                          const ClassField *inherited,
                                                          const ClassDecl *owner,
                                                          const ClassDecl *cls) {
  if (field.access > inherited->access) {
    const char *baseAccess = ClassBases::accessName(inherited->access);
    const char *access = ClassBases::accessName(field.access);
    std::string message =
        ErrorTextRedeclared::narrower(field.name, baseAccess, owner->name, access, cls->name);

    return err(ClassBasesAccessKeptError{
        .code = ClassBasesAccessKeptErrorCode::NARROWER_ACCESS,
        .message = message,
    });
  }

  return ok(&field);
}

ClassBases::ResultValueOnRedeclare ClassBases::valueGiven(const ClassDecl *cls,
                                                          const ClassField &field,
                                                          const ClassDecl *owner) {
  if (!field.init && !field.parameter() && cls->constructors.empty()) {
    std::string message = ErrorTextRedeclared::noValue(field.name, owner->name);

    return err(ClassBasesValueGivenError{
        .code = ClassBasesValueGivenErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&field);
}

void ClassBases::resolveClassBases(ClassDecl *cls) {
  cls->stage = ClassStage::RESOLVING_BASE;
  if (cls->base) {
    const TypeRef &ref = *cls->base;
    bool named = ref.kind() == TypeRefKind::NAME_REF || ref.kind() == TypeRefKind::GENERIC_REF;
    Stmt *found = named ? this->typeDeclarer->lookupType(ref.name) : nullptr;
    auto *parent = llvm::dyn_cast_or_null<ClassDecl>(found);
    bool instance = false;
    if (parent && parent->library() && parent->name == "EventEmitter" &&
        ref.kind() == TypeRefKind::NAME_REF) {
      Type *events = this->eventMapInference->classEventMap(cls);
      ClassDecl *emitter = parent;
      GenericClassInstantiator::ResultClassMade made =
          this->genericClassInstantiator->instantiateClass(emitter, {events}, ref.loc);
      if (made.isErr() && made.error().code !=
                              GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
        llvm::SMLoc at =
            made.error().code ==
                    GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
                ? emitter->loc()
                : ref.loc;
        this->state.getDiagnostics().error(at, made.error().message);
      }
      parent = made.isOk() ? made.value() : nullptr;
      instance = true;
      if (!parent) {
        found = nullptr;
      }
    } else if (parent && (parent->isGeneric() || ref.kind() == TypeRefKind::GENERIC_REF)) {
      const Type *type = this->typeResolver->resolveType(ref, false);
      parent = ClassDeclarer::classOf(type);
      instance = true;
      if (!parent) {
        found = nullptr;
      }
    }
    ResultParentClassFound base = this->baseClass(cls, ref, named, instance, parent, found);
    if (base.isErr() && base.error().code != ClassBasesBaseClassErrorCode::ALREADY_REPORTED) {
      this->state.getDiagnostics().error(ref.loc, base.error().message);
    }
    if (base.isOk()) {
      this->state.getScopeStack().enterOnlyTypes(this->state.getScopeStack().getTopTypes());
      if (Module *home = this->state.getModuleStates().findDeclModule(parent);
          home && home != this->state.getModuleStates().getCurrentModule() &&
          this->state.getScopeStack().isTopLevel()) {
        Module *here = this->state.getModuleStates().getCurrentModule();
        this->moduleDeclarer->useModule(home);
        this->classDeclarer->resolveClass(parent);
        this->moduleDeclarer->useModule(here);
      } else {
        this->classDeclarer->resolveClass(parent);
      }
      this->state.getScopeStack().leaveOnlyTypes();
      cls->parent = parent;
      parent->children.push_back(cls);
      this->addBase(cls->type, parent->type);
    }
  }
  for (const TypeRef &ref : cls->interfaces) {
    const Type *type = this->typeResolver->resolveType(ref, false);
    if (type->isError()) {
      continue;
    }
    ResultImplementedShape shape = ClassBases::implementedType(type, cls);
    if (shape.isErr()) {
      this->state.getDiagnostics().error(ref.loc, shape.error().message);
    } else if (!llvm::is_contained(cls->implemented, type)) {
      cls->implemented.push_back(type);
      this->addBase(cls->type, type);
    }
  }
  cls->stage = ClassStage::RESOLVED;

  for (ClassField &f : cls->fields) {
    if (f.isStatic()) {
      continue;
    }
    const ClassDecl *owner = nullptr;
    const ClassField *inherited =
        cls->parent ? ClassBases::findField(cls->parent, f.name, &owner) : nullptr;
    if (inherited && inherited->method()) {
      continue;
    }
    if (!inherited) {
      ResultOverrideTargeted target = ClassBases::overrideTarget(cls, f);
      if (target.isErr()) {
        this->state.getDiagnostics().error(f.loc, target.error().message);
      }
      continue;
    }
    ResultInheritedVisible visible = ClassBases::inheritedVisible(f, inherited, owner);
    if (visible.isErr()) {
      this->state.getDiagnostics().error(f.loc, visible.error().message);
      continue;
    }
    ResultAccessIsNotLower access = ClassBases::accessKept(f, inherited, owner, cls);
    if (access.isErr()) {
      this->state.getDiagnostics().error(f.loc, access.error().message);
    }
    ResultValueOnRedeclare valued = ClassBases::valueGiven(cls, f, owner);
    if (valued.isErr()) {
      this->state.getDiagnostics().error(f.loc, valued.error().message);
    }
    f.redeclared = true;
  }
}

ClassBases::ResultInterfaceNotSelf ClassBases::notSelf(const TypeAliasDecl *alias,
                                                       const TypeAliasDecl *other) {
  if (other && (other == alias || other->resolving)) {
    llvm::StringRef through = other == alias ? llvm::StringRef() : other->name;
    std::string message = ErrorTextInterfaceBases::itself(alias->name, through);

    return err(ClassBasesNotSelfError{
        .code = ClassBasesNotSelfErrorCode::EXTENDS_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

ClassBases::ResultObjectTypeAsBase ClassBases::interfaceBase(const Type *base) {
  if (base->isClass()) {
    std::string message = ErrorTextInterfaceBases::classBase(base->name());

    return err(ClassBasesInterfaceBaseError{
        .code = ClassBasesInterfaceBaseErrorCode::CLASS_BASE,
        .message = message,
    });
  }
  bool shape =
      base->isObject() && !base->isTuple() && !base->isIncomplete() && !base->isAnyObject();
  if (!shape) {
    std::string message = ErrorTextInterfaceBases::notIface(base->name());

    return err(ClassBasesInterfaceBaseError{
        .code = ClassBasesInterfaceBaseErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(base);
}

ClassBases::ResultMethodSignatures ClassBases::sameSignatures(const TypeAliasDecl *alias,
                                                              const Overload &known,
                                                              const Overload &other) {
  if (known.signatures != other.signatures) {
    std::string message = ErrorTextInterfaceBases::twoMethods(alias->name, other.name);

    return err(ClassBasesSameSignaturesError{
        .code = ClassBasesSameSignaturesErrorCode::OVERLOAD_CONFLICT,
        .message = message,
    });
  }

  return ok(&known);
}

ClassBases::ResultFieldTypesAreOne ClassBases::sameFieldType(const TypeAliasDecl *alias,
                                                             const Field &known, const Field &field,
                                                             llvm::StringRef origin,
                                                             llvm::StringRef baseName) {
  if (known.type != field.type) {
    std::string message = ErrorTextInterfaceBases::twoFields(
        alias->name, field.name, origin, known.type->name(), baseName, field.type->name());

    return err(ClassBasesSameFieldTypeError{
        .code = ClassBasesSameFieldTypeErrorCode::FIELD_CONFLICT,
        .message = message,
    });
  }

  return ok(&known);
}

ClassBases::ResultUnusedMethodName ClassBases::newOverload(const TypeAliasDecl *alias,
                                                           const Overload &overload,
                                                           llvm::ArrayRef<Field> fields,
                                                           llvm::ArrayRef<Overload> overloads) {
  bool clash = llvm::any_of(fields, [&](const Field &f) { return f.name == overload.name; }) ||
               llvm::any_of(overloads, [&](const Overload &p) { return p.name == overload.name; });
  if (clash) {
    std::string message = ErrorTextInterfaceBases::method(alias->name, overload.name);

    return err(ClassBasesNewOverloadError{
        .code = ClassBasesNewOverloadErrorCode::REDECLARED_METHOD,
        .message = message,
    });
  }

  return ok(&overload);
}

ClassBases::ResultUnusedMemberName ClassBases::newField(const TypeAliasDecl *alias,
                                                        const Field &field,
                                                        llvm::ArrayRef<Overload> overloads) {
  if (llvm::any_of(overloads, [&](const Overload &o) { return o.name == field.name; })) {
    std::string message = ErrorTextInterfaceBases::member(alias->name, field.name);

    return err(ClassBasesNewFieldError{
        .code = ClassBasesNewFieldErrorCode::REDECLARED_MEMBER,
        .message = message,
    });
  }

  return ok(&field);
}

ClassBases::ResultFieldNarrowsDown ClassBases::narrowsField(const TypeAliasDecl *alias,
                                                            const Field &field, const Field &known,
                                                            llvm::StringRef origin) {
  bool widened = known.type != field.type && !field.type->isError() && !known.type->isError() &&
                 !WideningRelations::sameReference(field.type, known.type);
  if (widened) {
    std::string message = ErrorTextInterfaceBases::narrowed(
        field.name, alias->name, field.type->name(), origin, known.type->name());

    return err(ClassBasesNarrowsFieldError{
        .code = ClassBasesNarrowsFieldErrorCode::WIDENED_FIELD,
        .message = message,
    });
  }

  return ok(&field);
}

std::vector<Field> ClassBases::interfaceFields(TypeAliasDecl *alias,
                                               std::vector<const Type *> &bases,
                                               std::vector<Overload> &overloads) {
  std::vector<Field> fields;
  std::vector<std::string> origins;
  for (const TypeRef &ref : alias->bases) {
    Stmt *found =
        ref.kind() == TypeRefKind::NAME_REF ? this->typeDeclarer->lookupType(ref.name) : nullptr;
    auto *other = llvm::dyn_cast_or_null<TypeAliasDecl>(found);
    ResultInterfaceNotSelf self = ClassBases::notSelf(alias, other);
    if (self.isErr()) {
      this->state.getDiagnostics().error(ref.loc, self.error().message);
      continue;
    }
    const Type *base = this->typeResolver->resolveType(ref, false);
    if (base->isError()) {
      continue;
    }
    ResultObjectTypeAsBase shape = ClassBases::interfaceBase(base);
    if (shape.isErr()) {
      this->state.getDiagnostics().error(ref.loc, shape.error().message);
      continue;
    }
    if (llvm::is_contained(bases, base)) {
      continue;
    }
    bases.push_back(base);
    for (const Overload &o : base->overloads()) {
      auto it = llvm::find_if(overloads, [&](const Overload &p) { return p.name == o.name; });
      if (it == overloads.end()) {
        overloads.push_back(o);
        continue;
      }
      ResultMethodSignatures signatures = ClassBases::sameSignatures(alias, *it, o);
      if (signatures.isErr()) {
        this->state.getDiagnostics().error(ref.loc, signatures.error().message);
      }
    }
    std::string baseName = ref.name.str();
    for (const Field &f : base->fields()) {
      auto it = llvm::find_if(fields, [&](const Field &g) { return g.name == f.name; });
      if (it == fields.end()) {
        fields.push_back(f);
        origins.push_back(baseName);
        continue;
      }
      const std::string &origin = origins[size_t(it - fields.begin())];
      ResultFieldTypesAreOne one = ClassBases::sameFieldType(alias, *it, f, origin, baseName);
      if (one.isErr()) {
        this->state.getDiagnostics().error(ref.loc, one.error().message);
      } else {
        it->readonly = it->readonly && f.readonly;
      }
    }
  }
  const TypeRef &own = alias->type;
  std::vector<Overload> ownOverloads;
  std::vector<Field> ownFields = this->typeResolver->resolveFields(own, &ownOverloads);
  for (const Overload &o : ownOverloads) {
    ResultUnusedMethodName unused = ClassBases::newOverload(alias, o, fields, overloads);
    if (unused.isErr()) {
      this->state.getDiagnostics().error(alias->loc(), unused.error().message);
    } else {
      overloads.push_back(o);
    }
  }
  for (Field &f : ownFields) {
    ResultUnusedMemberName unused = ClassBases::newField(alias, f, overloads);
    if (unused.isErr()) {
      this->state.getDiagnostics().error(alias->loc(), unused.error().message);
      continue;
    }
    llvm::SMLoc loc = alias->loc();
    for (size_t i = 0; i < own.names().size(); ++i) {
      if (own.names()[i] == f.name) {
        loc = own.locs()[i];
      }
    }
    auto it = llvm::find_if(fields, [&](const Field &g) { return g.name == f.name; });
    if (it == fields.end()) {
      fields.push_back(f);
      origins.push_back(alias->name.str());
      continue;
    }
    const std::string &origin = origins[size_t(it - fields.begin())];
    ResultFieldNarrowsDown narrows = ClassBases::narrowsField(alias, f, *it, origin);
    if (narrows.isErr()) {
      this->state.getDiagnostics().error(loc, narrows.error().message);
      continue;
    }
    if (it->type != f.type || (f.readonly && !it->readonly)) {
      this->state.getWideningStore().markFieldsNarrowed();
    }
    it->type = f.type;
    it->readonly = f.readonly;
  }

  return fields;
}

const ClassField *ClassBases::findField(const ClassDecl *cls, llvm::StringRef name,
                                        const ClassDecl **owner) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (const ClassField *f = c->field(name)) {
      if (owner) {
        *owner = c;
      }
      return f;
    }
  }

  return nullptr;
}

}
