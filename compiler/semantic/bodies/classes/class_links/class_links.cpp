#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver_errors.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ClassLinks::ClassLinks(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->chunkTypes = &all.chunkTypes;
  this->classBases = &all.classBases;
  this->classDeclarer = &all.classDeclarer;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool ClassLinks::isSameParams(const FuncSymbol *first, const FuncSymbol *second) const {
  return first->params == second->params && first->minArgs == second->minArgs &&
         first->hasRest == second->hasRest;
}

bool ClassLinks::isTakingParams(const Type *functionType, const FuncSymbol *function) const {
  return functionType->isFunction() &&
         functionType->params() == llvm::ArrayRef<const Type *>(function->params) &&
         functionType->minArgs() == function->minArgs &&
         functionType->hasRest() == function->hasRest;
}

bool ClassLinks::isArrowMethod(const FunctionDecl *functionDecl) const {
  if (functionDecl->cls() == nullptr) {
    return false;
  }
  for (const ClassField &field : functionDecl->cls()->fields) {
    if (field.method() == functionDecl) {
      return true;
    }
  }

  return false;
}

bool ClassLinks::isDeclaringReturn(const FunctionDecl *functionDecl) const {
  if (functionDecl->returnType.has_value()) {
    return true;
  }
  if (functionDecl->cls() == nullptr) {
    return false;
  }
  for (const ClassField &field : functionDecl->cls()->fields) {
    if (field.method() == functionDecl && field.type.has_value()) {
      return true;
    }
  }

  return false;
}

void ClassLinks::interfacesOf(const ClassDecl *classDecl, std::vector<const Type *> &out) const {
  std::vector<const Type *> queue(classDecl->implemented.begin(), classDecl->implemented.end());
  for (size_t index = 0; index < queue.size(); ++index) {
    const Type *next = queue[index];
    if (!llvm::is_contained(out, next)) {
      this->addInterface(next, queue, out);
    }
  }
}

void ClassLinks::addInterface(const Type *interfaceType, std::vector<const Type *> &queue,
                              std::vector<const Type *> &out) const {
  out.push_back(interfaceType);
  for (const Type *base : interfaceType->bases()) {
    queue.push_back(base);
  }
}

std::vector<FunctionDecl *> ClassLinks::constructorsOf(const ClassDecl *classDecl) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    if (!current->constructors.empty()) {
      return current->constructors;
    }
  }

  return {};
}

std::vector<FunctionDecl *> ClassLinks::ownMethods(const ClassDecl *classDecl) const {
  auto methods = std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->isStatic()) {
      methods.push_back(functionDecl);
    }
  }
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

bool ClassLinks::isOverridingTransitively(const FunctionDecl *functionDecl,
                                          const FunctionDecl *base) const {
  for (const FunctionDecl *current = functionDecl; current != nullptr;
       current = current->overrides()) {
    if (current == base) {
      return true;
    }
  }

  return false;
}

FunctionDecl *ClassLinks::soleImplementation(const ClassDecl *classDecl,
                                             FunctionDecl *variant) const {
  if (variant->openToLate()) {
    return nullptr;
  }
  if (!variant->overridden() && !variant->isAbstract()) {
    return variant;
  }
  if (variant->generic != nullptr && variant->overridden()) {
    return nullptr;
  }
  auto implementations = this->implementationsBelow(classDecl, variant);
  FunctionDecl *only = variant->isAbstract() ? nullptr : variant;
  unsigned count = only != nullptr ? 1 : 0;
  for (const OwnedMethod &implementation : implementations) {
    only = implementation.method;
    count += implementation.owner->isEmitted() ? 1 : 2;
  }

  return count == 1 ? only : nullptr;
}

std::vector<ClassLinks::OwnedMethod>
ClassLinks::implementationsBelow(const ClassDecl *classDecl, const FunctionDecl *variant) const {
  auto implementations = std::vector<OwnedMethod>();
  if (classDecl == nullptr) {
    return implementations;
  }
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    this->addImplementations(child, variant, implementations);
  }

  return implementations;
}

void ClassLinks::addImplementations(const ClassDecl *child, const FunctionDecl *variant,
                                    std::vector<OwnedMethod> &implementations) const {
  for (FunctionDecl *functionDecl : this->ownMethods(child)) {
    bool isImplementing = !functionDecl->isAbstract() && functionDecl != variant &&
                          this->isOverridingTransitively(functionDecl, variant);
    if (isImplementing) {
      implementations.push_back(OwnedMethod{.method = functionDecl, .owner = child});
    }
  }
}

FunctionDecl *ClassLinks::firstImplementation(const ClassDecl *classDecl,
                                              const FunctionDecl *functionDecl) const {
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    auto *implementation = this->ownImplementation(child, functionDecl);
    if (implementation != nullptr) {
      return implementation;
    }
  }

  return nullptr;
}

FunctionDecl *ClassLinks::ownImplementation(const ClassDecl *child,
                                            const FunctionDecl *functionDecl) const {
  for (FunctionDecl *implementation : this->ownMethods(child)) {
    bool isImplementing = !implementation->isAbstract() &&
                          this->isOverridingTransitively(implementation, functionDecl);
    if (isImplementing) {
      return implementation;
    }
  }

  return nullptr;
}

const Type *ClassLinks::commonClass(const Type *first, const Type *second) {
  const ClassDecl *firstClass = first->classDecl();
  const ClassDecl *secondClass = second->classDecl();
  if (firstClass == nullptr || secondClass == nullptr) {
    return nullptr;
  }
  for (const ClassDecl *current = firstClass; current != nullptr; current = current->parent) {
    if (secondClass->derivesFrom(current)) {
      return current->type;
    }
  }

  return nullptr;
}

void ClassLinks::linkClasses() {
  auto order = ClassTree::treeOrder(this->state.getClassStore().getClasses());
  for (ClassDecl *classDecl : order) {
    this->linkOverrides(classDecl);
  }
  this->numberClasses();
  for (ClassDecl *classDecl : order) {
    this->buildVtable(classDecl);
  }
  this->state.getClassStore().markClassesLinked();
  auto sources = llvm::SmallPtrSet<const FunctionDecl *, 4>();
  for (ClassDecl *classDecl : order) {
    if (classDecl->generic() == nullptr) {
      this->linkReturnTypes(classDecl, sources);
    }
  }
}

void ClassLinks::linkReturnTypes(const ClassDecl *classDecl,
                                 llvm::SmallPtrSet<const FunctionDecl *, 4> &sources) {
  for (FunctionDecl *functionDecl : this->ownMethods(classDecl)) {
    bool isSettled = functionDecl->sym == nullptr || this->isDeclaringReturn(functionDecl) ||
                     sources.contains(functionDecl);
    if (!isSettled) {
      this->linkReturnType(classDecl, functionDecl, sources);
    }
  }
}

void ClassLinks::linkReturnType(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                                llvm::SmallPtrSet<const FunctionDecl *, 4> &sources) {
  if (functionDecl->isAbstract() && functionDecl->overrides() == nullptr) {
    return this->linkAbstractReturn(classDecl, functionDecl, sources);
  }
  if (functionDecl->overrides() != nullptr) {
    return this->linkOverrideReturn(functionDecl);
  }

  return this->linkInterfaceReturn(classDecl, functionDecl);
}

void ClassLinks::linkAbstractReturn(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                                    llvm::SmallPtrSet<const FunctionDecl *, 4> &sources) {
  auto *source = this->firstImplementation(classDecl, functionDecl);
  functionDecl->sym->setReturnType(nullptr);
  this->state.getClassStore().rememberReturnSource(functionDecl, source);
  if (source != nullptr) {
    sources.insert(source);
  }
}

void ClassLinks::linkOverrideReturn(FunctionDecl *functionDecl) {
  FunctionDecl *base = functionDecl->overrides();
  bool isBaseTyped =
      this->isDeclaringReturn(base) && base->sym != nullptr && base->sym->hasReturnType();
  if (isBaseTyped) {
    functionDecl->sym->setReturnType(base->sym->getKnownReturnType());
    return;
  }
  functionDecl->sym->setReturnType(nullptr);
  this->state.getClassStore().rememberInheritedReturn(functionDecl, base);
}

void ClassLinks::linkInterfaceReturn(const ClassDecl *classDecl, FunctionDecl *functionDecl) {
  auto interfaces = std::vector<const Type *>();
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    this->interfacesOf(current, interfaces);
  }
  for (const Type *interfaceType : interfaces) {
    if (this->linkInterfaceField(interfaceType, functionDecl)) {
      break;
    }
    this->linkInterfaceOverloads(interfaceType, functionDecl);
  }
}

bool ClassLinks::linkInterfaceField(const Type *interfaceType, FunctionDecl *functionDecl) {
  const auto *member = interfaceType->field(functionDecl->name);
  if (member == nullptr || !this->isTakingParams(member->type, functionDecl->sym)) {
    return false;
  }
  functionDecl->sym->setReturnType(member->type->ret());

  return true;
}

void ClassLinks::linkInterfaceOverloads(const Type *interfaceType, FunctionDecl *functionDecl) {
  const auto *overload = interfaceType->overload(functionDecl->name);
  if (overload == nullptr) {
    return;
  }
  for (const Type *signature : overload->signatures) {
    if (this->isTakingParams(signature, functionDecl->sym)) {
      functionDecl->sym->setReturnType(signature->ret());
    }
  }
}

void ClassLinks::numberClasses() {
  auto order = ClassTree::treeOrder(this->state.getClassStore().getClasses());
  unsigned next = 0;
  for (ClassDecl *classDecl : order) {
    classDecl->id = ++next;
  }
  for (ClassDecl *classDecl : llvm::reverse(order)) {
    classDecl->lastId = this->lastDescendantId(classDecl);
  }
  for (ClassDecl *classDecl : this->state.getClassStore().getClasses()) {
    if (classDecl->literal() != nullptr) {
      classDecl->id = ++next;
      classDecl->lastId = classDecl->id;
    }
  }
  this->state.getClassStore().rememberClassCount(next);
}

unsigned ClassLinks::lastDescendantId(const ClassDecl *classDecl) const {
  unsigned last = classDecl->id;
  for (const ClassDecl *child : classDecl->children) {
    last = std::max(last, child->lastId);
  }

  return last;
}

void ClassLinks::buildVtable(ClassDecl *classDecl) {
  classDecl->vtable =
      classDecl->parent != nullptr ? classDecl->parent->vtable : std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : this->ownMethods(classDecl)) {
    this->placeInVtable(classDecl, functionDecl);
  }
}

void ClassLinks::placeInVtable(ClassDecl *classDecl, FunctionDecl *functionDecl) {
  Overriding *overriding = functionDecl->overriding();
  FunctionDecl *entry = functionDecl->isAbstract() ? nullptr : functionDecl;
  if (functionDecl->overrides() != nullptr) {
    overriding->slot = functionDecl->overrides()->slot();
    bool isInside =
        functionDecl->slot() >= 0 && size_t(functionDecl->slot()) < classDecl->vtable.size();
    if (isInside) {
      classDecl->vtable[size_t(functionDecl->slot())] = entry;
    }
    return;
  }
  if (functionDecl->overridden() || functionDecl->isAbstract()) {
    overriding->slot = int(classDecl->vtable.size());
    classDecl->vtable.push_back(entry);
  }
}

ClassLinks::ResultMethodNotField
ClassLinks::checkNotAField(const ClassDecl *classDecl, const FunctionDecl *functionDecl) const {
  const ClassDecl *owner = nullptr;
  const ClassField *field =
      classDecl->parent != nullptr
          ? this->classBases->findField(classDecl->parent, functionDecl->name, &owner)
          : nullptr;
  if (field != nullptr && field->method() == nullptr) {
    auto message = ErrorTextNotAField::fieldOf(functionDecl->name, owner->name);

    return err(ClassLinksNotAFieldError{
        .code = ClassLinksNotAFieldErrorCode::FIELD_OF_PARENT,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ClassLinks::ResultOverrideTarget
ClassLinks::checkOverrideTarget(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                llvm::ArrayRef<FunctionDecl *> inherited) const {
  if (functionDecl->isOverride() && classDecl->parent == nullptr) {
    auto message = ErrorTextOverrideTarget::noParent(functionDecl->name, classDecl->name);

    return err(ClassLinksOverrideTargetError{
        .code = ClassLinksOverrideTargetErrorCode::NO_PARENT,
        .message = message,
    });
  }
  if (functionDecl->isOverride() && inherited.empty()) {
    auto message = ErrorTextOverrideTarget::noMethod(functionDecl->name, classDecl->parent->name);

    return err(ClassLinksOverrideTargetError{
        .code = ClassLinksOverrideTargetErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }
  if (functionDecl->isOverride()) {
    auto list = ErrorTextVariant::list(inherited);
    auto message = ErrorTextOverrideTarget::noParams(functionDecl->name, classDecl->parent->name,
                                                     inherited.size() == 1, list);

    return err(ClassLinksOverrideTargetError{
        .code = ClassLinksOverrideTargetErrorCode::NO_MATCHING_PARAMS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ClassLinks::ResultBaseIsNotArrow ClassLinks::checkArrowOverride(const FunctionDecl *functionDecl,
                                                                const FunctionDecl *base) const {
  if (this->isArrowMethod(base) && !this->isArrowMethod(functionDecl)) {
    auto message = ErrorTextArrowOverride::arrow(functionDecl->name, base->cls()->name);

    return err(ClassLinksArrowOverrideError{
        .code = ClassLinksArrowOverrideErrorCode::ARROW_IN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

ClassLinks::ResultBaseNotPrivate ClassLinks::checkBaseVisible(const FunctionDecl *functionDecl,
                                                              const FunctionDecl *base) const {
  if (base->access() == Access::Private) {
    auto message = ErrorTextBaseVisible::hidden(functionDecl->name, base->cls()->name);

    return err(ClassLinksBaseVisibleError{
        .code = ClassLinksBaseVisibleErrorCode::PRIVATE_IN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

ClassLinks::ResultAccessNotLower ClassLinks::checkAccessKept(const FunctionDecl *functionDecl,
                                                             const FunctionDecl *base) const {
  if (functionDecl->access() > base->access()) {
    auto baseAccess = AccessKeywordTable::getName(base->access());
    auto access = AccessKeywordTable::getName(functionDecl->access());
    auto message =
        ErrorTextAccessKept::narrower(functionDecl->name, baseAccess, base->cls()->name, access);

    return err(ClassLinksAccessKeptError{
        .code = ClassLinksAccessKeptErrorCode::NARROWER_ACCESS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ClassLinks::ResultBodyIsRetained ClassLinks::checkBodyKept(const ClassDecl *classDecl,
                                                           const FunctionDecl *functionDecl,
                                                           const FunctionDecl *base) const {
  if (functionDecl->isAbstract() && !base->isAbstract()) {
    auto message = ErrorTextBodyKept::again(functionDecl->name, base->cls()->name, classDecl->name);

    return err(ClassLinksBodyKeptError{
        .code = ClassLinksBodyKeptErrorCode::ABSTRACT_AGAIN,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ClassLinks::ResultSameTypeParams ClassLinks::checkSameTypeArgs(const ClassDecl *classDecl,
                                                               const FunctionDecl *functionDecl,
                                                               const FunctionDecl *base) const {
  if (this->state.getClassStore().hasLinkedClasses() && !base->overridden() && base->slot() < 0) {
    auto message =
        ErrorTextSameTypeArgs::typeArgs(functionDecl->name, classDecl->type->name(),
                                        base->cls()->type->name(), base->name, base->cls()->name);

    return err(ClassLinksSameTypeArgsError{
        .code = ClassLinksSameTypeArgsErrorCode::OVERRIDE_WITH_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

void ClassLinks::linkOverrides(ClassDecl *classDecl) {
  bool isInstance = classDecl->generic() != nullptr;
  if (isInstance) {
    this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  }
  for (FunctionDecl *functionDecl : this->ownMethods(classDecl)) {
    if (functionDecl->sym != nullptr) {
      this->linkOverride(classDecl, functionDecl);
    }
  }
  if (isInstance) {
    this->state.getDiagnostics().popList();
  }
}

void ClassLinks::linkOverride(ClassDecl *classDecl, FunctionDecl *functionDecl) {
  auto inherited = classDecl->parent != nullptr
                       ? this->classDeclarer->methodsNamed(classDecl->parent, functionDecl->name)
                       : std::vector<FunctionDecl *>();
  auto *base = this->sameParamsBase(inherited, functionDecl);
  auto method = this->checkNotAField(classDecl, functionDecl);
  if (method.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), method.error().message);
    return;
  }
  if (base == nullptr) {
    return this->walkNewMethod(classDecl, functionDecl, inherited);
  }
  if (this->reportBadOverride(classDecl, functionDecl, base)) {
    return;
  }
  functionDecl->overriding()->overrides = base;
  this->walkOverrideChain(classDecl, functionDecl, base);
}

FunctionDecl *ClassLinks::sameParamsBase(llvm::ArrayRef<FunctionDecl *> inherited,
                                         const FunctionDecl *functionDecl) const {
  for (FunctionDecl *candidate : llvm::reverse(inherited)) {
    if (candidate->sym != nullptr && this->isSameParams(candidate->sym, functionDecl->sym)) {
      return candidate;
    }
  }

  return nullptr;
}

void ClassLinks::walkNewMethod(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                               llvm::ArrayRef<FunctionDecl *> inherited) {
  auto target = this->checkOverrideTarget(classDecl, functionDecl, inherited);
  if (target.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), target.error().message);
  }
  if (!inherited.empty() && this->chunkTypes->isLinkedStreamHook(functionDecl)) {
    return this->walkHookParams(functionDecl, inherited);
  }
  for (FunctionDecl *inheritedMethod : inherited) {
    this->classDeclarer->reportVariantPair(inheritedMethod, functionDecl);
  }
}

void ClassLinks::walkHookParams(const FunctionDecl *functionDecl,
                                llvm::ArrayRef<FunctionDecl *> inherited) {
  auto params = this->chunkTypes->checkHookParams(functionDecl, inherited);
  if (params.isErr() && params.error().code == ChunkTypesHookParamsErrorCode::OTHER_PARAMETERS) {
    this->state.getDiagnostics().error(functionDecl->loc(), params.error().message);
  }
}

bool ClassLinks::reportBadOverride(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                   const FunctionDecl *base) {
  auto notArrow = this->checkArrowOverride(functionDecl, base);
  if (notArrow.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), notArrow.error().message);
    return true;
  }
  auto visible = this->checkBaseVisible(functionDecl, base);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), visible.error().message);
    return true;
  }
  auto access = this->checkAccessKept(functionDecl, base);
  if (access.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), access.error().message);
  }
  auto body = this->checkBodyKept(classDecl, functionDecl, base);
  if (body.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), body.error().message);
  }

  return false;
}

void ClassLinks::walkOverrideChain(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                   FunctionDecl *base) {
  for (FunctionDecl *current = base; current != nullptr; current = current->overrides()) {
    auto same = this->checkSameTypeArgs(classDecl, functionDecl, current);
    if (same.isErr()) {
      this->state.getDiagnostics().errorOutsideTrials(functionDecl->loc(), same.error().message);
    }
    this->genericClassInstantiator->markOverridden(current);
  }
}

const Type *ClassLinks::inheritedReturnType(FuncSymbol *function) {
  FunctionDecl *functionDecl = function->decl;
  if (functionDecl == nullptr) {
    return nullptr;
  }
  auto &classes = this->state.getClassStore();
  if (classes.hasReturnSource(functionDecl)) {
    auto *source = classes.findReturnSource(functionDecl);
    return source != nullptr ? source->sym->getReturnType() : Type::voidTy();
  }
  auto *base = classes.findInheritedReturn(functionDecl);
  if (base != nullptr) {
    return base->sym->getReturnType();
  }

  return nullptr;
}

void ClassLinks::noteInheritedReturn(const FunctionDecl *functionDecl) {
  bool isInherited = functionDecl != nullptr && functionDecl->cls() != nullptr &&
                     !functionDecl->returnType.has_value() && functionDecl->sym != nullptr &&
                     functionDecl->sym->hasReturnType();
  if (!isInherited) {
    return;
  }
  std::string type = functionDecl->sym->getKnownReturnType()->name().str();
  const FunctionDecl *base = functionDecl->overrides();
  if (base != nullptr) {
    auto returns = ErrorTextInheritedReturn::returns(functionDecl->name, base->cls()->name, type);
    this->state.getDiagnostics().note(base->loc(), returns);
  }
}

}
