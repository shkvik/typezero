#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"
#include "compiler/program/library/builtin_names/error_class_table/error_class_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
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
  this->fsDeclarer = &all.fsDeclarer;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->classDeclarer = &all.classDeclarer;
  this->moduleDeclarer = &all.moduleDeclarer;
  this->eventMapInference = &all.eventMapInference;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool ClassBases::isErrorClass(llvm::StringRef name) const {
  return ErrorClassTable::isErrorClass(name);
}

void ClassBases::addBase(const Type *type, const Type *base) {
  const_cast<Type *>(type)->addBase(base);
  bool isEmitted = !this->state.getFunctionStack().isInsideTrial() &&
                   llvm::none_of(this->state.getFunctionStack().getFunctions(),
                                 std::bind_front(&ClassBases::isUninstantiatedGeneric, this));
  if (isEmitted) {
    this->state.getInventoryStore().addSubtype(type);
  }
}

bool ClassBases::isUninstantiatedGeneric(const FunctionDecl *functionDecl) const {
  return !functionDecl->typeParams.empty() && !functionDecl->isInstance;
}

ClassBases::ResultParentClassFound ClassBases::checkBaseClass(const ClassDecl *classDecl,
                                                              const TypeRef &ref, bool isNamed,
                                                              const ParentLookup &lookup) const {
  if (!isNamed) {
    auto message = ErrorTextExtends::byName();

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::NOT_NAMED,
        .message = message,
    });
  }
  if (lookup.isInstance && lookup.parent == nullptr) {
    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (lookup.parent != nullptr) {
    return this->checkUsableParent(classDecl, lookup.parent);
  }

  return this->checkMissingParent(classDecl, ref, lookup.found);
}

ClassBases::ResultParentClassFound ClassBases::checkUsableParent(const ClassDecl *classDecl,
                                                                 ClassDecl *parent) const {
  if (parent == classDecl || parent->stage == ClassStage::RESOLVING_BASE) {
    llvm::StringRef through = parent == classDecl ? llvm::StringRef() : parent->name;
    auto message = ErrorTextExtends::itself(classDecl->name, through);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::EXTENDS_ITSELF,
        .message = message,
    });
  }
  if (this->fsDeclarer->isRuntimeClass(parent)) {
    auto message = ErrorTextExtends::runtime(classDecl->name, parent->name);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (parent->baseExpr != nullptr && parent->stage == ClassStage::UNRESOLVED) {
    auto message = ErrorTextExtends::valueBase(classDecl->name, parent->name);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::VALUE_BASE,
        .message = message,
    });
  }

  return ok(parent);
}

ClassBases::ResultParentClassFound ClassBases::checkMissingParent(const ClassDecl *classDecl,
                                                                  const TypeRef &ref,
                                                                  const Stmt *found) const {
  if (found == nullptr && this->isErrorClass(ref.name)) {
    auto message = ErrorTextExtends::errorClass(ref.name, classDecl->name);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::ERROR_CLASS,
        .message = message,
    });
  }
  if (found != nullptr) {
    bool isAlias = isa<TypeAliasDecl>(found);
    auto message = ErrorTextExtends::notClass(ref.name, classDecl->name, isAlias);

    return err(ClassBasesBaseClassError{
        .code = ClassBasesBaseClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }
  auto message = ErrorTextExtends::noClass(ref.name);

  return err(ClassBasesBaseClassError{
      .code = ClassBasesBaseClassErrorCode::NO_SUCH_CLASS,
      .message = message,
  });
}

ClassBases::ResultImplementedShape
ClassBases::checkImplementedType(const Type *type, const ClassDecl *classDecl) const {
  if (type->isClass()) {
    auto message = ErrorTextExtends::classIface(type->name(), classDecl->base.has_value());

    return err(ClassBasesImplementedTypeError{
        .code = ClassBasesImplementedTypeErrorCode::CLASS_AS_INTERFACE,
        .message = message,
    });
  }
  bool isShape =
      type->isObject() && !type->isTuple() && !type->isAnyObject() && !type->isIncomplete();
  if (!isShape) {
    auto message = ErrorTextExtends::notIface(type->name());

    return err(ClassBasesImplementedTypeError{
        .code = ClassBasesImplementedTypeErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(type);
}

ClassBases::ResultOverrideTargeted ClassBases::checkOverrideTarget(const ClassDecl *classDecl,
                                                                   const ClassField &field) const {
  if (field.isOverride && classDecl->parent != nullptr) {
    auto message = ErrorTextRedeclared::noField(field.name, classDecl->parent->name);

    return err(ClassBasesOverrideTargetError{
        .code = ClassBasesOverrideTargetErrorCode::NO_FIELD,
        .message = message,
    });
  }
  if (field.isOverride) {
    auto message = ErrorTextRedeclared::noParent(field.name, classDecl->name);

    return err(ClassBasesOverrideTargetError{
        .code = ClassBasesOverrideTargetErrorCode::NO_PARENT,
        .message = message,
    });
  }

  return ok(&field);
}

ClassBases::ResultInheritedVisible ClassBases::checkInheritedVisible(const ClassField &field,
                                                                     const ClassField *inherited,
                                                                     const ClassDecl *owner) const {
  if (inherited->access == Access::Private) {
    auto message = ErrorTextRedeclared::isPrivate(field.name, owner->name);

    return err(ClassBasesInheritedVisibleError{
        .code = ClassBasesInheritedVisibleErrorCode::PRIVATE_IN_BASE,
        .message = message,
    });
  }

  return ok(inherited);
}

ClassBases::ResultAccessIsNotLower ClassBases::checkAccessKept(const ClassField &field,
                                                               const ClassField *inherited,
                                                               const ClassDecl *owner,
                                                               const ClassDecl *classDecl) const {
  if (field.access > inherited->access) {
    auto baseAccess = AccessKeywordTable::getName(inherited->access);
    auto access = AccessKeywordTable::getName(field.access);
    auto message =
        ErrorTextRedeclared::narrower(field.name, baseAccess, owner->name, access, classDecl->name);

    return err(ClassBasesAccessKeptError{
        .code = ClassBasesAccessKeptErrorCode::NARROWER_ACCESS,
        .message = message,
    });
  }

  return ok(&field);
}

ClassBases::ResultValueOnRedeclare ClassBases::checkValueGiven(const ClassDecl *classDecl,
                                                               const ClassField &field,
                                                               const ClassDecl *owner) const {
  if (field.init == nullptr && !field.parameter() && classDecl->constructors.empty()) {
    auto message = ErrorTextRedeclared::noValue(field.name, owner->name);

    return err(ClassBasesValueGivenError{
        .code = ClassBasesValueGivenErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&field);
}

void ClassBases::resolveClassBases(ClassDecl *classDecl) {
  classDecl->stage = ClassStage::RESOLVING_BASE;
  if (classDecl->base.has_value()) {
    this->resolveParentClass(classDecl, *classDecl->base);
  }
  for (const TypeRef &ref : classDecl->interfaces) {
    this->addImplemented(classDecl, ref);
  }
  classDecl->stage = ClassStage::RESOLVED;
  for (ClassField &field : classDecl->fields) {
    this->walkRedeclaredField(classDecl, field);
  }
}

void ClassBases::resolveParentClass(ClassDecl *classDecl, const TypeRef &ref) {
  bool isNamed = ref.kind() == TypeRefKind::NAME_REF || ref.kind() == TypeRefKind::GENERIC_REF;
  Stmt *found = isNamed ? this->typeDeclarer->lookupType(ref.name) : nullptr;
  auto lookup = this->parentOf(classDecl, ref, found);
  auto base = this->checkBaseClass(classDecl, ref, isNamed, lookup);
  if (base.isErr() && base.error().code != ClassBasesBaseClassErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(ref.loc, base.error().message);
  }
  if (base.isErr()) {
    return;
  }

  return this->linkParent(classDecl, lookup.parent);
}

ClassBases::ParentLookup ClassBases::parentOf(ClassDecl *classDecl, const TypeRef &ref,
                                              Stmt *found) {
  auto *parent = llvm::dyn_cast_or_null<ClassDecl>(found);
  bool isEmitter = parent != nullptr && parent->library() &&
                   LibraryClassTable::isName(parent->name, LibraryClass::EVENT_EMITTER) &&
                   ref.kind() == TypeRefKind::NAME_REF;
  if (isEmitter) {
    return this->emitterParent(classDecl, ref, parent, found);
  }
  bool isGeneric =
      parent != nullptr && (parent->isGeneric() || ref.kind() == TypeRefKind::GENERIC_REF);
  if (!isGeneric) {
    return ParentLookup{
        .parent = parent,
        .found = found,
        .isInstance = false,
    };
  }
  const auto *type = this->typeResolver->resolveType(ref);
  auto *instance = this->classDeclarer->classOf(type);

  return ParentLookup{
      .parent = instance,
      .found = instance != nullptr ? found : nullptr,
      .isInstance = true,
  };
}

ClassBases::ParentLookup ClassBases::emitterParent(ClassDecl *classDecl, const TypeRef &ref,
                                                   ClassDecl *emitter, Stmt *found) {
  auto *events = this->eventMapInference->classEventMap(classDecl);
  auto made = this->genericClassInstantiator->instantiateClass(emitter, {events}, ref.loc);
  bool isReportable =
      made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED;
  if (isReportable) {
    bool isEndless =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES;
    llvm::SMLoc at = isEndless ? emitter->loc() : ref.loc;
    this->state.getDiagnostics().error(at, made.error().message);
  }
  ClassDecl *parent = made.isOk() ? made.value() : nullptr;

  return ParentLookup{
      .parent = parent,
      .found = parent != nullptr ? found : nullptr,
      .isInstance = true,
  };
}

void ClassBases::linkParent(ClassDecl *classDecl, ClassDecl *parent) {
  auto topTypes = this->state.getScopeStack().getTopTypes();
  this->state.getScopeStack().enterOnlyTypes(topTypes);
  auto *home = this->state.getModuleStates().findDeclModule(parent);
  auto *here = this->state.getModuleStates().getCurrentModule();
  bool isForeign = home != nullptr && home != here && this->state.getScopeStack().isTopLevel();
  if (isForeign) {
    this->moduleDeclarer->useModule(home);
    this->classDeclarer->resolveClass(parent);
    this->moduleDeclarer->useModule(here);
  } else {
    this->classDeclarer->resolveClass(parent);
  }
  this->state.getScopeStack().leaveOnlyTypes();
  classDecl->parent = parent;
  parent->children.push_back(classDecl);
  this->addBase(classDecl->type, parent->type);
}

void ClassBases::addImplemented(ClassDecl *classDecl, const TypeRef &ref) {
  const auto *type = this->typeResolver->resolveType(ref);
  if (type->isError()) {
    return;
  }
  auto shape = this->checkImplementedType(type, classDecl);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(ref.loc, shape.error().message);
    return;
  }
  if (llvm::is_contained(classDecl->implemented, type)) {
    return;
  }
  classDecl->implemented.push_back(type);
  this->addBase(classDecl->type, type);
}

void ClassBases::walkRedeclaredField(ClassDecl *classDecl, ClassField &field) {
  if (field.isStatic()) {
    return;
  }
  const ClassDecl *owner = nullptr;
  const ClassField *inherited = classDecl->parent != nullptr
                                    ? this->findField(classDecl->parent, field.name, &owner)
                                    : nullptr;
  if (inherited != nullptr && inherited->method() != nullptr) {
    return;
  }
  if (inherited == nullptr) {
    auto target = this->checkOverrideTarget(classDecl, field);
    if (target.isErr()) {
      this->state.getDiagnostics().error(field.loc, target.error().message);
    }
    return;
  }
  auto visible = this->checkInheritedVisible(field, inherited, owner);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(field.loc, visible.error().message);
    return;
  }
  auto access = this->checkAccessKept(field, inherited, owner, classDecl);
  if (access.isErr()) {
    this->state.getDiagnostics().error(field.loc, access.error().message);
  }
  auto valued = this->checkValueGiven(classDecl, field, owner);
  if (valued.isErr()) {
    this->state.getDiagnostics().error(field.loc, valued.error().message);
  }
  field.redeclared = true;
}

ClassBases::ResultInterfaceNotSelf ClassBases::checkNotSelf(const TypeAliasDecl *alias,
                                                            const TypeAliasDecl *other) const {
  if (other != nullptr && (other == alias || other->resolving)) {
    llvm::StringRef through = other == alias ? llvm::StringRef() : other->name;
    auto message = ErrorTextInterfaceBases::itself(alias->name, through);

    return err(ClassBasesNotSelfError{
        .code = ClassBasesNotSelfErrorCode::EXTENDS_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

ClassBases::ResultObjectTypeAsBase ClassBases::checkInterfaceBase(const Type *base) const {
  if (base->isClass()) {
    auto message = ErrorTextInterfaceBases::classBase(base->name());

    return err(ClassBasesInterfaceBaseError{
        .code = ClassBasesInterfaceBaseErrorCode::CLASS_BASE,
        .message = message,
    });
  }
  bool isShape =
      base->isObject() && !base->isTuple() && !base->isIncomplete() && !base->isAnyObject();
  if (!isShape) {
    auto message = ErrorTextInterfaceBases::notIface(base->name());

    return err(ClassBasesInterfaceBaseError{
        .code = ClassBasesInterfaceBaseErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(base);
}

ClassBases::ResultMethodSignatures ClassBases::checkSameSignatures(const TypeAliasDecl *alias,
                                                                   const Overload &known,
                                                                   const Overload &other) const {
  if (known.signatures != other.signatures) {
    auto message = ErrorTextInterfaceBases::twoMethods(alias->name, other.name);

    return err(ClassBasesSameSignaturesError{
        .code = ClassBasesSameSignaturesErrorCode::OVERLOAD_CONFLICT,
        .message = message,
    });
  }

  return ok(&known);
}

ClassBases::ResultFieldTypesAreOne
ClassBases::checkSameFieldType(const TypeAliasDecl *alias, const Field &known, const Field &field,
                               llvm::StringRef origin, llvm::StringRef baseName) const {
  if (known.type != field.type) {
    auto message = ErrorTextInterfaceBases::twoFields(
        alias->name, field.name, origin, known.type->name(), baseName, field.type->name());

    return err(ClassBasesSameFieldTypeError{
        .code = ClassBasesSameFieldTypeErrorCode::FIELD_CONFLICT,
        .message = message,
    });
  }

  return ok(&known);
}

ClassBases::ResultUnusedMethodName
ClassBases::checkNewOverload(const TypeAliasDecl *alias, const Overload &overload,
                             llvm::ArrayRef<Field> fields,
                             llvm::ArrayRef<Overload> overloads) const {
  bool isClash = this->fieldIndex(fields, overload.name) != fields.size() ||
                 this->overloadNamed(overloads, overload.name) != nullptr;
  if (isClash) {
    auto message = ErrorTextInterfaceBases::method(alias->name, overload.name);

    return err(ClassBasesNewOverloadError{
        .code = ClassBasesNewOverloadErrorCode::REDECLARED_METHOD,
        .message = message,
    });
  }

  return ok(&overload);
}

ClassBases::ResultUnusedMemberName
ClassBases::checkNewField(const TypeAliasDecl *alias, const Field &field,
                          llvm::ArrayRef<Overload> overloads) const {
  if (this->overloadNamed(overloads, field.name) != nullptr) {
    auto message = ErrorTextInterfaceBases::member(alias->name, field.name);

    return err(ClassBasesNewFieldError{
        .code = ClassBasesNewFieldErrorCode::REDECLARED_MEMBER,
        .message = message,
    });
  }

  return ok(&field);
}

ClassBases::ResultFieldNarrowsDown ClassBases::checkNarrowsField(const TypeAliasDecl *alias,
                                                                 const Field &field,
                                                                 const Field &known,
                                                                 llvm::StringRef origin) const {
  bool isWidened = known.type != field.type && !field.type->isError() && !known.type->isError() &&
                   !WideningRelations::isSameReference(field.type, known.type);
  if (isWidened) {
    auto message = ErrorTextInterfaceBases::narrowed(field.name, alias->name, field.type->name(),
                                                     origin, known.type->name());

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
  auto shape = InterfaceShape();
  for (const TypeRef &ref : alias->bases) {
    const auto *base = this->interfaceBaseType(alias, ref, bases);
    if (base != nullptr) {
      bases.push_back(base);
      this->mergeBaseOverloads(alias, ref, base, overloads);
      this->mergeBaseFields(alias, ref, base, shape);
    }
  }
  auto ownOverloads = std::vector<Overload>();
  auto ownFields = this->typeResolver->resolveFields(alias->type, &ownOverloads);
  for (const Overload &overload : ownOverloads) {
    auto unused = this->checkNewOverload(alias, overload, shape.fields, overloads);
    if (unused.isErr()) {
      this->state.getDiagnostics().error(alias->loc(), unused.error().message);
    } else {
      overloads.push_back(overload);
    }
  }
  for (const Field &field : ownFields) {
    this->addOwnField(alias, field, overloads, shape);
  }

  return shape.fields;
}

const Type *ClassBases::interfaceBaseType(const TypeAliasDecl *alias, const TypeRef &ref,
                                          llvm::ArrayRef<const Type *> bases) {
  Stmt *found =
      ref.kind() == TypeRefKind::NAME_REF ? this->typeDeclarer->lookupType(ref.name) : nullptr;
  auto *other = llvm::dyn_cast_or_null<TypeAliasDecl>(found);
  auto self = this->checkNotSelf(alias, other);
  if (self.isErr()) {
    this->state.getDiagnostics().error(ref.loc, self.error().message);
    return nullptr;
  }
  const auto *base = this->typeResolver->resolveType(ref);
  if (base->isError()) {
    return nullptr;
  }
  auto shape = this->checkInterfaceBase(base);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(ref.loc, shape.error().message);
    return nullptr;
  }
  if (llvm::is_contained(bases, base)) {
    return nullptr;
  }

  return base;
}

void ClassBases::mergeBaseOverloads(const TypeAliasDecl *alias, const TypeRef &ref,
                                    const Type *base, std::vector<Overload> &overloads) {
  for (const Overload &overload : base->overloads()) {
    const auto *known = this->overloadNamed(overloads, overload.name);
    if (known == nullptr) {
      overloads.push_back(overload);
    } else {
      this->walkSameSignatures(alias, ref, *known, overload);
    }
  }
}

void ClassBases::walkSameSignatures(const TypeAliasDecl *alias, const TypeRef &ref,
                                    const Overload &known, const Overload &overload) {
  auto signatures = this->checkSameSignatures(alias, known, overload);
  if (signatures.isErr()) {
    this->state.getDiagnostics().error(ref.loc, signatures.error().message);
  }
}

const Overload *ClassBases::overloadNamed(llvm::ArrayRef<Overload> overloads,
                                          llvm::StringRef name) const {
  for (const Overload &overload : overloads) {
    if (overload.name == name) {
      return &overload;
    }
  }

  return nullptr;
}

size_t ClassBases::fieldIndex(llvm::ArrayRef<Field> fields, llvm::StringRef name) const {
  for (size_t index = 0; index < fields.size(); ++index) {
    if (fields[index].name == name) {
      return index;
    }
  }

  return fields.size();
}

void ClassBases::mergeBaseFields(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                                 InterfaceShape &shape) {
  std::string baseName = ref.name.str();
  for (const Field &field : base->fields()) {
    this->mergeBaseField(alias, ref, field, baseName, shape);
  }
}

void ClassBases::mergeBaseField(const TypeAliasDecl *alias, const TypeRef &ref, const Field &field,
                                const std::string &baseName, InterfaceShape &shape) {
  size_t at = this->fieldIndex(shape.fields, field.name);
  if (at == shape.fields.size()) {
    shape.fields.push_back(field);
    shape.origins.push_back(baseName);
    return;
  }
  Field &known = shape.fields[at];
  auto one = this->checkSameFieldType(alias, known, field, shape.origins[at], baseName);
  if (one.isErr()) {
    this->state.getDiagnostics().error(ref.loc, one.error().message);
    return;
  }
  known.readonly = known.readonly && field.readonly;
}

void ClassBases::addOwnField(const TypeAliasDecl *alias, const Field &field,
                             llvm::ArrayRef<Overload> overloads, InterfaceShape &shape) {
  auto unused = this->checkNewField(alias, field, overloads);
  if (unused.isErr()) {
    this->state.getDiagnostics().error(alias->loc(), unused.error().message);
    return;
  }
  auto loc = this->ownFieldLoc(alias, field.name);
  size_t at = this->fieldIndex(shape.fields, field.name);
  if (at == shape.fields.size()) {
    shape.fields.push_back(field);
    shape.origins.push_back(alias->name.str());
    return;
  }
  Field &known = shape.fields[at];
  auto narrows = this->checkNarrowsField(alias, field, known, shape.origins[at]);
  if (narrows.isErr()) {
    this->state.getDiagnostics().error(loc, narrows.error().message);
    return;
  }
  if (known.type != field.type || (field.readonly && !known.readonly)) {
    this->state.getWideningStore().markFieldsNarrowed();
  }
  known.type = field.type;
  known.readonly = field.readonly;
}

llvm::SMLoc ClassBases::ownFieldLoc(const TypeAliasDecl *alias, llvm::StringRef name) const {
  const TypeRef &own = alias->type;
  for (size_t index = own.names().size(); index > 0; --index) {
    if (own.names()[index - 1] == name) {
      return own.locs()[index - 1];
    }
  }

  return alias->loc();
}

const ClassField *ClassBases::findField(const ClassDecl *classDecl, llvm::StringRef name,
                                        const ClassDecl **owner) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    const auto *field = current->field(name);
    if (field != nullptr && owner != nullptr) {
      *owner = current;
    }
    if (field != nullptr) {
      return field;
    }
  }

  return nullptr;
}

}
