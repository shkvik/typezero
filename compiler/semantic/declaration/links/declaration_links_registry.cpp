#include "compiler/semantic/declaration/links/declaration_links_registry.h"
#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver_errors.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

DeclarationLinksRegistry::DeclarationLinksRegistry(SemanticState &owner)
    : state(owner) {}

bool DeclarationLinksRegistry::isSameParams(const FuncSymbol *first,
                                            const FuncSymbol *second) const {
  return first->params == second->params && first->minArgs == second->minArgs &&
         first->hasRest == second->hasRest;
}

bool DeclarationLinksRegistry::isTakingParams(const Type *functionType,
                                              const FuncSymbol *function) const {
  return functionType->isFunction() &&
         functionType->params() == llvm::ArrayRef<const Type *>(function->params) &&
         functionType->minArgs() == function->minArgs &&
         functionType->hasRest() == function->hasRest;
}

bool DeclarationLinksRegistry::isArrowMethod(const FunctionDecl *functionDecl) const {
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

bool DeclarationLinksRegistry::isDeclaringReturn(const FunctionDecl *functionDecl) const {
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

void DeclarationLinksRegistry::interfacesOf(const ClassDecl *classDecl,
                                            std::vector<const Type *> &out) const {
  std::vector<const Type *> queue(classDecl->implemented.begin(), classDecl->implemented.end());
  for (size_t index = 0; index < queue.size(); ++index) {
    const Type *next = queue[index];
    if (!llvm::is_contained(out, next)) {
      this->addInterface(next, queue, out);
    }
  }
}

void DeclarationLinksRegistry::addInterface(const Type *interfaceType,
                                            std::vector<const Type *> &queue,
                                            std::vector<const Type *> &out) const {
  out.push_back(interfaceType);
  for (const Type *base : interfaceType->bases()) {
    queue.push_back(base);
  }
}

std::vector<FunctionDecl *> DeclarationLinksRegistry::ownMethods(const ClassDecl *classDecl) const {
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

bool DeclarationLinksRegistry::isOverridingTransitively(const FunctionDecl *functionDecl,
                                                        const FunctionDecl *base) const {
  for (const FunctionDecl *current = functionDecl; current != nullptr;
       current = current->overrides()) {
    if (current == base) {
      return true;
    }
  }

  return false;
}

FunctionDecl *
DeclarationLinksRegistry::firstImplementation(const ClassDecl *classDecl,
                                              const FunctionDecl *functionDecl) const {
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    auto *implementation = this->ownImplementation(child, functionDecl);
    if (implementation != nullptr) {
      return implementation;
    }
  }

  return nullptr;
}

FunctionDecl *DeclarationLinksRegistry::ownImplementation(const ClassDecl *child,
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

void DeclarationLinksRegistry::startLinkClasses() {
  auto order = ClassTree::treeOrder(this->state.declaration.classes.getClasses());
  this->state.walk.tasks.pushTask(FinishLinkClasses{.order = order});
  for (ClassDecl *classDecl : llvm::reverse(order)) {
    this->state.walk.tasks.pushTask(StartLinkOverrides{.classDecl = classDecl});
  }
}

void DeclarationLinksRegistry::finishLinkClasses(const FinishLinkClasses &task) {
  this->numberClasses();
  for (ClassDecl *classDecl : task.order) {
    this->buildVtable(classDecl);
  }
  this->state.declaration.classes.markClassesLinked();
  auto sources = llvm::SmallPtrSet<const FunctionDecl *, 4>();
  for (ClassDecl *classDecl : task.order) {
    if (classDecl->generic() == nullptr) {
      this->linkReturnTypes(classDecl, sources);
    }
  }
}

void DeclarationLinksRegistry::linkReturnTypes(
    const ClassDecl *classDecl, llvm::SmallPtrSet<const FunctionDecl *, 4> &sources) {
  for (FunctionDecl *functionDecl : this->ownMethods(classDecl)) {
    bool isSettled = functionDecl->sym == nullptr || this->isDeclaringReturn(functionDecl) ||
                     sources.contains(functionDecl);
    if (!isSettled) {
      this->linkReturnType(classDecl, functionDecl, sources);
    }
  }
}

void DeclarationLinksRegistry::linkReturnType(const ClassDecl *classDecl,
                                              FunctionDecl *functionDecl,
                                              llvm::SmallPtrSet<const FunctionDecl *, 4> &sources) {
  if (functionDecl->isAbstract() && functionDecl->overrides() == nullptr) {
    return this->linkAbstractReturn(classDecl, functionDecl, sources);
  }
  if (functionDecl->overrides() != nullptr) {
    return this->linkOverrideReturn(functionDecl);
  }

  return this->linkInterfaceReturn(classDecl, functionDecl);
}

void DeclarationLinksRegistry::linkAbstractReturn(
    const ClassDecl *classDecl, FunctionDecl *functionDecl,
    llvm::SmallPtrSet<const FunctionDecl *, 4> &sources) {
  auto *source = this->firstImplementation(classDecl, functionDecl);
  functionDecl->sym->setReturnType(nullptr);
  this->state.inference.returns.rememberReturnSource(functionDecl, source);
  if (source != nullptr) {
    sources.insert(source);
  }
}

void DeclarationLinksRegistry::linkOverrideReturn(FunctionDecl *functionDecl) {
  FunctionDecl *base = functionDecl->overrides();
  bool isBaseTyped =
      this->isDeclaringReturn(base) && base->sym != nullptr && base->sym->hasReturnType();
  if (isBaseTyped) {
    functionDecl->sym->setReturnType(base->sym->getKnownReturnType());
    return;
  }
  functionDecl->sym->setReturnType(nullptr);
  this->state.inference.returns.rememberInheritedReturn(functionDecl);
}

void DeclarationLinksRegistry::linkInterfaceReturn(const ClassDecl *classDecl,
                                                   FunctionDecl *functionDecl) {
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

bool DeclarationLinksRegistry::linkInterfaceField(const Type *interfaceType,
                                                  FunctionDecl *functionDecl) {
  const auto *member = interfaceType->field(functionDecl->name);
  if (member == nullptr || !this->isTakingParams(member->type, functionDecl->sym)) {
    return false;
  }
  functionDecl->sym->setReturnType(member->type->ret());

  return true;
}

void DeclarationLinksRegistry::linkInterfaceOverloads(const Type *interfaceType,
                                                      FunctionDecl *functionDecl) {
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

void DeclarationLinksRegistry::numberClasses() {
  auto order = ClassTree::treeOrder(this->state.declaration.classes.getClasses());
  unsigned next = 0;
  for (ClassDecl *classDecl : order) {
    classDecl->id = ++next;
  }
  for (ClassDecl *classDecl : llvm::reverse(order)) {
    classDecl->lastId = this->lastDescendantId(classDecl);
  }
  for (ClassDecl *classDecl : this->state.declaration.classes.getClasses()) {
    if (classDecl->literal() != nullptr) {
      classDecl->id = ++next;
      classDecl->lastId = classDecl->id;
    }
  }
  this->state.declaration.classes.rememberClassCount(next);
}

unsigned DeclarationLinksRegistry::lastDescendantId(const ClassDecl *classDecl) const {
  unsigned last = classDecl->id;
  for (const ClassDecl *child : classDecl->children) {
    last = std::max(last, child->lastId);
  }

  return last;
}

void DeclarationLinksRegistry::buildVtable(ClassDecl *classDecl) {
  classDecl->vtable =
      classDecl->parent != nullptr ? classDecl->parent->vtable : std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : this->ownMethods(classDecl)) {
    this->placeInVtable(classDecl, functionDecl);
  }
}

void DeclarationLinksRegistry::placeInVtable(ClassDecl *classDecl, FunctionDecl *functionDecl) {
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

DeclarationLinksRegistry::ResultMethodNotField
DeclarationLinksRegistry::checkNotAField(const ClassDecl *classDecl,
                                         const FunctionDecl *functionDecl) const {
  const auto *field = classDecl->findInheritedField(functionDecl->name);
  const auto *owner = classDecl->findInheritedFieldOwner(functionDecl->name);
  if (field != nullptr && field->method() == nullptr) {
    auto message = ErrorTextNotAField::fieldOf(functionDecl->name, owner->name);

    return err(DeclarationLinksRegistryNotAFieldError{
        .code = DeclarationLinksRegistryNotAFieldErrorCode::FIELD_OF_PARENT,
        .message = message,
    });
  }

  return ok(functionDecl);
}

DeclarationLinksRegistry::ResultOverrideTarget
DeclarationLinksRegistry::checkOverrideTarget(const ClassDecl *classDecl,
                                              const FunctionDecl *functionDecl,
                                              llvm::ArrayRef<FunctionDecl *> inherited) const {
  if (functionDecl->isOverride() && classDecl->parent == nullptr) {
    auto message = ErrorTextOverrideTarget::noParent(functionDecl->name, classDecl->name);

    return err(DeclarationLinksRegistryOverrideTargetError{
        .code = DeclarationLinksRegistryOverrideTargetErrorCode::NO_PARENT,
        .message = message,
    });
  }
  if (functionDecl->isOverride() && inherited.empty()) {
    auto message = ErrorTextOverrideTarget::noMethod(functionDecl->name, classDecl->parent->name);

    return err(DeclarationLinksRegistryOverrideTargetError{
        .code = DeclarationLinksRegistryOverrideTargetErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }
  if (functionDecl->isOverride()) {
    auto list = ErrorTextVariant::list(inherited);
    auto message = ErrorTextOverrideTarget::noParams(functionDecl->name, classDecl->parent->name,
                                                     inherited.size() == 1, list);

    return err(DeclarationLinksRegistryOverrideTargetError{
        .code = DeclarationLinksRegistryOverrideTargetErrorCode::NO_MATCHING_PARAMS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

DeclarationLinksRegistry::ResultBaseIsNotArrow
DeclarationLinksRegistry::checkArrowOverride(const FunctionDecl *functionDecl,
                                             const FunctionDecl *base) const {
  if (this->isArrowMethod(base) && !this->isArrowMethod(functionDecl)) {
    auto message = ErrorTextArrowOverride::arrow(functionDecl->name, base->cls()->name);

    return err(DeclarationLinksRegistryArrowOverrideError{
        .code = DeclarationLinksRegistryArrowOverrideErrorCode::ARROW_IN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

DeclarationLinksRegistry::ResultBaseNotPrivate
DeclarationLinksRegistry::checkBaseVisible(const FunctionDecl *functionDecl,
                                           const FunctionDecl *base) const {
  if (base->access() == Access::Private) {
    auto message = ErrorTextBaseVisible::hidden(functionDecl->name, base->cls()->name);

    return err(DeclarationLinksRegistryBaseVisibleError{
        .code = DeclarationLinksRegistryBaseVisibleErrorCode::PRIVATE_IN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

DeclarationLinksRegistry::ResultAccessNotLower
DeclarationLinksRegistry::checkAccessKept(const FunctionDecl *functionDecl,
                                          const FunctionDecl *base) const {
  if (functionDecl->access() > base->access()) {
    auto baseAccess = AccessKeywordTable::getName(base->access());
    auto access = AccessKeywordTable::getName(functionDecl->access());
    auto message =
        ErrorTextAccessKept::narrower(functionDecl->name, baseAccess, base->cls()->name, access);

    return err(DeclarationLinksRegistryAccessKeptError{
        .code = DeclarationLinksRegistryAccessKeptErrorCode::NARROWER_ACCESS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

DeclarationLinksRegistry::ResultBodyIsRetained DeclarationLinksRegistry::checkBodyKept(
    const ClassDecl *classDecl, const FunctionDecl *functionDecl, const FunctionDecl *base) const {
  if (functionDecl->isAbstract() && !base->isAbstract()) {
    auto message = ErrorTextBodyKept::again(functionDecl->name, base->cls()->name, classDecl->name);

    return err(DeclarationLinksRegistryBodyKeptError{
        .code = DeclarationLinksRegistryBodyKeptErrorCode::ABSTRACT_AGAIN,
        .message = message,
    });
  }

  return ok(functionDecl);
}

DeclarationLinksRegistry::ResultSameTypeParams DeclarationLinksRegistry::checkSameTypeArgs(
    const ClassDecl *classDecl, const FunctionDecl *functionDecl, const FunctionDecl *base) const {
  if (this->state.declaration.classes.hasLinkedClasses() && !base->overridden() &&
      base->slot() < 0) {
    auto message =
        ErrorTextSameTypeArgs::typeArgs(functionDecl->name, classDecl->type->name(),
                                        base->cls()->type->name(), base->name, base->cls()->name);

    return err(DeclarationLinksRegistrySameTypeArgsError{
        .code = DeclarationLinksRegistrySameTypeArgsErrorCode::OVERRIDE_WITH_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

void DeclarationLinksRegistry::startLinkOverrides(const StartLinkOverrides &task) {
  ClassDecl *classDecl = task.classDecl;
  bool isInstance = classDecl->generic() != nullptr;
  if (isInstance) {
    this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  }
  this->state.walk.tasks.pushTask(FinishLinkOverrides{.classDecl = classDecl});
  auto methods = this->ownMethods(classDecl);
  for (FunctionDecl *functionDecl : llvm::reverse(methods)) {
    if (functionDecl->sym != nullptr) {
      this->state.walk.tasks.pushTask(
          LinkOverride{.classDecl = classDecl, .functionDecl = functionDecl});
    }
  }
}

void DeclarationLinksRegistry::finishLinkOverrides(const FinishLinkOverrides &task) {
  bool isInstance = task.classDecl->generic() != nullptr;
  if (isInstance) {
    this->state.diagnostics.popList();
  }
}

void DeclarationLinksRegistry::linkOverride(const LinkOverride &task) {
  ClassDecl *classDecl = task.classDecl;
  FunctionDecl *functionDecl = task.functionDecl;
  auto inherited = classDecl->parent != nullptr
                       ? classDecl->parent->findMethodsNamed(functionDecl->name)
                       : std::vector<FunctionDecl *>();
  auto *base = this->sameParamsBase(inherited, functionDecl);
  auto method = this->checkNotAField(classDecl, functionDecl);
  if (method.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), method.error().message);
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

FunctionDecl *DeclarationLinksRegistry::sameParamsBase(llvm::ArrayRef<FunctionDecl *> inherited,
                                                       const FunctionDecl *functionDecl) const {
  for (FunctionDecl *candidate : llvm::reverse(inherited)) {
    if (candidate->sym != nullptr && this->isSameParams(candidate->sym, functionDecl->sym)) {
      return candidate;
    }
  }

  return nullptr;
}

void DeclarationLinksRegistry::walkNewMethod(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                                             const std::vector<FunctionDecl *> &inherited) {
  auto target = this->checkOverrideTarget(classDecl, functionDecl, inherited);
  if (target.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), target.error().message);
  }
  bool isHook =
      !inherited.empty() && this->state.library.declarations.isLinkedStreamHook(functionDecl);
  if (isHook) {
    return this->state.walk.tasks.pushTask(
        WalkHookParams{.functionDecl = functionDecl, .inherited = inherited});
  }
  for (FunctionDecl *inheritedMethod : llvm::reverse(inherited)) {
    this->state.walk.tasks.pushTask(
        ReportVariantPair{.first = inheritedMethod, .second = functionDecl});
  }
}

bool DeclarationLinksRegistry::reportBadOverride(const ClassDecl *classDecl,
                                                 const FunctionDecl *functionDecl,
                                                 const FunctionDecl *base) {
  auto notArrow = this->checkArrowOverride(functionDecl, base);
  if (notArrow.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), notArrow.error().message);
    return true;
  }
  auto visible = this->checkBaseVisible(functionDecl, base);
  if (visible.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), visible.error().message);
    return true;
  }
  auto access = this->checkAccessKept(functionDecl, base);
  if (access.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), access.error().message);
  }
  auto body = this->checkBodyKept(classDecl, functionDecl, base);
  if (body.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), body.error().message);
  }

  return false;
}

void DeclarationLinksRegistry::walkOverrideChain(const ClassDecl *classDecl,
                                                 const FunctionDecl *functionDecl,
                                                 FunctionDecl *base) {
  for (FunctionDecl *current = base; current != nullptr; current = current->overrides()) {
    auto same = this->checkSameTypeArgs(classDecl, functionDecl, current);
    if (same.isErr()) {
      this->state.diagnostics.errorOutsideTrials(functionDecl->loc(), same.error().message);
    }
    this->state.walk.tasks.pushTask(MarkOverridden{.functionDecl = current});
  }
}

}
