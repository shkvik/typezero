#include "compiler/semantic/bodies/classes/class_links/class_links.h"
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

bool ClassLinks::sameParams(const FuncSymbol *a, const FuncSymbol *b) const {
  return a->params == b->params && a->minArgs == b->minArgs && a->hasRest == b->hasRest;
}

bool ClassLinks::takesParams(const Type *functionType, const FuncSymbol *function) const {
  return functionType->isFunction() &&
         functionType->params() == llvm::ArrayRef<const Type *>(function->params) &&
         functionType->minArgs() == function->minArgs &&
         functionType->hasRest() == function->hasRest;
}

const char *ClassLinks::accessName(Access access) const {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
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

bool ClassLinks::declaresReturn(const FunctionDecl *functionDecl) const {
  if (functionDecl->returnType) {
    return true;
  }
  if (functionDecl->cls() == nullptr) {
    return false;
  }
  for (const ClassField &field : functionDecl->cls()->fields) {
    if (field.method() == functionDecl && field.type) {
      return true;
    }
  }

  return false;
}

void ClassLinks::interfacesOf(const ClassDecl *classDecl, std::vector<const Type *> &out) const {
  std::vector<const Type *> queue(classDecl->implemented.begin(), classDecl->implemented.end());
  for (size_t index = 0; index < queue.size(); ++index) {
    if (llvm::is_contained(out, queue[index])) {
      continue;
    }
    out.push_back(queue[index]);
    for (const Type *base : queue[index]->bases()) {
      queue.push_back(base);
    }
  }
}

std::vector<FunctionDecl *> ClassLinks::constructorsOf(const ClassDecl *classDecl) const {
  for (const ClassDecl *c = classDecl; c != nullptr; c = c->parent) {
    if (!c->constructors.empty()) {
      return c->constructors;
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

bool ClassLinks::overridesTransitively(const FunctionDecl *functionDecl,
                                       const FunctionDecl *base) const {
  for (const FunctionDecl *p = functionDecl; p != nullptr; p = p->overrides()) {
    if (p == base) {
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
  FunctionDecl *only = variant->isAbstract() ? nullptr : variant;
  unsigned count = only != nullptr ? 1 : 0;
  std::vector<ClassDecl *> below =
      classDecl != nullptr ? ClassTree::descendants(classDecl) : std::vector<ClassDecl *>();
  for (const ClassDecl *child : below) {
    for (FunctionDecl *functionDecl : this->ownMethods(child)) {
      if (!functionDecl->isAbstract() && functionDecl != variant &&
          this->overridesTransitively(functionDecl, variant)) {
        only = functionDecl;
        count += child->isEmitted() ? 1 : 2;
      }
    }
  }

  return count == 1 ? only : nullptr;
}

FunctionDecl *ClassLinks::firstImplementation(const ClassDecl *classDecl,
                                              const FunctionDecl *functionDecl) const {
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    for (FunctionDecl *implementation : this->ownMethods(child)) {
      if (!implementation->isAbstract() &&
          this->overridesTransitively(implementation, functionDecl)) {
        return implementation;
      }
    }
  }

  return nullptr;
}

const Type *ClassLinks::commonClass(const Type *a, const Type *b) {
  const ClassDecl *x = a->classDecl();
  const ClassDecl *y = b->classDecl();
  if (x == nullptr || y == nullptr) {
    return nullptr;
  }
  for (const ClassDecl *classDecl = x; classDecl != nullptr; classDecl = classDecl->parent) {
    if (y->derivesFrom(classDecl)) {
      return classDecl->type;
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
    if (classDecl->generic() != nullptr) {
      continue;
    }
    for (FunctionDecl *functionDecl : this->ownMethods(classDecl)) {
      if (functionDecl->sym == nullptr || this->declaresReturn(functionDecl) ||
          sources.contains(functionDecl)) {
        continue;
      }
      if (functionDecl->isAbstract() && functionDecl->overrides() == nullptr) {
        auto *source = this->firstImplementation(classDecl, functionDecl);
        functionDecl->sym->setReturnType(nullptr);
        this->state.getClassStore().rememberReturnSource(functionDecl, source);
        if (source != nullptr) {
          sources.insert(source);
        }
        continue;
      }
      if (functionDecl->overrides() != nullptr) {
        FunctionDecl *base = functionDecl->overrides();
        if (this->declaresReturn(base) && base->sym != nullptr && base->sym->hasReturnType()) {
          functionDecl->sym->setReturnType(base->sym->getKnownReturnType());
        } else {
          functionDecl->sym->setReturnType(nullptr);
          this->state.getClassStore().rememberInheritedReturn(functionDecl, base);
        }
        continue;
      }
      auto interfaces = std::vector<const Type *>();
      for (const ClassDecl *c = classDecl; c != nullptr; c = c->parent) {
        this->interfacesOf(c, interfaces);
      }
      for (const Type *interfaceType : interfaces) {
        const auto *member = interfaceType->field(functionDecl->name);
        if (member != nullptr && this->takesParams(member->type, functionDecl->sym)) {
          functionDecl->sym->setReturnType(member->type->ret());
          break;
        }
        const auto *overload = interfaceType->overload(functionDecl->name);
        if (overload != nullptr) {
          for (const Type *signature : overload->signatures) {
            if (this->takesParams(signature, functionDecl->sym)) {
              functionDecl->sym->setReturnType(signature->ret());
            }
          }
        }
      }
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
    classDecl->lastId = classDecl->id;
    for (const ClassDecl *child : classDecl->children) {
      classDecl->lastId = std::max(classDecl->lastId, child->lastId);
    }
  }
  for (ClassDecl *classDecl : this->state.getClassStore().getClasses()) {
    if (classDecl->literal() != nullptr) {
      classDecl->id = classDecl->lastId = ++next;
    }
  }
  this->state.getClassStore().rememberClassCount(next);
}

void ClassLinks::buildVtable(ClassDecl *classDecl) {
  classDecl->vtable =
      classDecl->parent != nullptr ? classDecl->parent->vtable : std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : this->ownMethods(classDecl)) {
    Overriding *overriding = functionDecl->overriding();
    if (functionDecl->overrides() != nullptr) {
      overriding->slot = functionDecl->overrides()->slot();
      if (functionDecl->slot() >= 0 && size_t(functionDecl->slot()) < classDecl->vtable.size()) {
        classDecl->vtable[size_t(functionDecl->slot())] =
            functionDecl->isAbstract() ? nullptr : functionDecl;
      }
      continue;
    }
    if (functionDecl->overridden() || functionDecl->isAbstract()) {
      overriding->slot = int(classDecl->vtable.size());
      classDecl->vtable.push_back(functionDecl->isAbstract() ? nullptr : functionDecl);
    }
  }
}

ClassLinks::ResultMethodNotField ClassLinks::notAField(const ClassDecl *classDecl,
                                                       const FunctionDecl *functionDecl) const {
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
ClassLinks::overrideTarget(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
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

ClassLinks::ResultBaseIsNotArrow ClassLinks::arrowOverride(const FunctionDecl *functionDecl,
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

ClassLinks::ResultBaseNotPrivate ClassLinks::baseVisible(const FunctionDecl *functionDecl,
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

ClassLinks::ResultAccessNotLower ClassLinks::accessKept(const FunctionDecl *functionDecl,
                                                        const FunctionDecl *base) const {
  if (functionDecl->access() > base->access()) {
    llvm::StringRef baseAccess = this->accessName(base->access());
    llvm::StringRef access = this->accessName(functionDecl->access());
    auto message =
        ErrorTextAccessKept::narrower(functionDecl->name, baseAccess, base->cls()->name, access);

    return err(ClassLinksAccessKeptError{
        .code = ClassLinksAccessKeptErrorCode::NARROWER_ACCESS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ClassLinks::ResultBodyIsRetained ClassLinks::bodyKept(const ClassDecl *classDecl,
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

ClassLinks::ResultSameTypeParams ClassLinks::sameTypeArgs(const ClassDecl *classDecl,
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
  auto inherited = std::vector<FunctionDecl *>();
  if (classDecl->parent != nullptr) {
    inherited = this->classDeclarer->methodsNamed(classDecl->parent, functionDecl->name);
  }
  auto *base = this->sameParamsBase(inherited, functionDecl);
  auto method = this->notAField(classDecl, functionDecl);
  if (method.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), method.error().message);
    return;
  }
  if (base == nullptr) {
    this->walkNewMethod(classDecl, functionDecl, inherited);
    return;
  }
  if (!this->overrideFits(classDecl, functionDecl, base)) {
    return;
  }
  functionDecl->overriding()->overrides = base;
  this->walkOverrideChain(classDecl, functionDecl, base);
}

FunctionDecl *ClassLinks::sameParamsBase(llvm::ArrayRef<FunctionDecl *> inherited,
                                         const FunctionDecl *functionDecl) const {
  FunctionDecl *base = nullptr;
  for (FunctionDecl *v : inherited) {
    if (v->sym != nullptr && this->sameParams(v->sym, functionDecl->sym)) {
      base = v;
    }
  }

  return base;
}

void ClassLinks::walkNewMethod(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                               llvm::ArrayRef<FunctionDecl *> inherited) {
  auto target = this->overrideTarget(classDecl, functionDecl, inherited);
  if (target.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), target.error().message);
  }
  if (!inherited.empty() && this->chunkTypes->isStreamHook(functionDecl, true)) {
    auto params = this->chunkTypes->hookParams(functionDecl, inherited);
    if (params.isErr() && params.error().code == ChunkTypesHookParamsErrorCode::OTHER_PARAMETERS) {
      this->state.getDiagnostics().error(functionDecl->loc(), params.error().message);
    }
    return;
  }
  for (FunctionDecl *v : inherited) {
    this->classDeclarer->reportVariantPair(v, functionDecl);
  }
}

bool ClassLinks::overrideFits(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                              const FunctionDecl *base) {
  auto notArrow = this->arrowOverride(functionDecl, base);
  if (notArrow.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), notArrow.error().message);
    return false;
  }
  auto visible = this->baseVisible(functionDecl, base);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), visible.error().message);
    return false;
  }
  auto access = this->accessKept(functionDecl, base);
  if (access.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), access.error().message);
  }
  auto body = this->bodyKept(classDecl, functionDecl, base);
  if (body.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), body.error().message);
  }

  return true;
}

void ClassLinks::walkOverrideChain(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                   FunctionDecl *base) {
  for (FunctionDecl *p = base; p != nullptr; p = p->overrides()) {
    auto same = this->sameTypeArgs(classDecl, functionDecl, p);
    if (same.isErr()) {
      this->state.getDiagnostics().errorOutsideTrials(functionDecl->loc(), same.error().message);
    }
    this->genericClassInstantiator->markOverridden(p);
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
  if (functionDecl == nullptr || functionDecl->cls() == nullptr || functionDecl->returnType ||
      functionDecl->sym == nullptr || !functionDecl->sym->hasReturnType()) {
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
