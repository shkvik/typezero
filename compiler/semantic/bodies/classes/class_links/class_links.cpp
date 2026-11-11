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
  this->classDeclarer = &all.classDeclarer;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool ClassLinks::sameParams(const FuncSymbol *a, const FuncSymbol *b) {
  return a->params == b->params && a->minArgs == b->minArgs && a->hasRest == b->hasRest;
}

bool ClassLinks::takesParams(const Type *fnType, const FuncSymbol *sym) {
  return fnType->isFunction() && fnType->params() == llvm::ArrayRef<const Type *>(sym->params) &&
         fnType->minArgs() == sym->minArgs && fnType->hasRest() == sym->hasRest;
}

const char *ClassLinks::accessName(Access access) {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
}

bool ClassLinks::isArrowMethod(const FunctionDecl *fn) {
  if (!fn->cls()) {
    return false;
  }
  for (const ClassField &f : fn->cls()->fields) {
    if (f.method() == fn) {
      return true;
    }
  }

  return false;
}

bool ClassLinks::declaresReturn(const FunctionDecl *fn) {
  if (fn->returnType) {
    return true;
  }
  if (!fn->cls()) {
    return false;
  }
  for (const ClassField &f : fn->cls()->fields) {
    if (f.method() == fn && f.type) {
      return true;
    }
  }

  return false;
}

void ClassLinks::interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out) {
  std::vector<const Type *> queue(cls->implemented.begin(), cls->implemented.end());
  for (size_t i = 0; i < queue.size(); ++i) {
    if (llvm::is_contained(out, queue[i])) {
      continue;
    }
    out.push_back(queue[i]);
    for (const Type *base : queue[i]->bases()) {
      queue.push_back(base);
    }
  }
}

std::vector<FunctionDecl *> ClassLinks::constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->constructors.empty()) {
      return c->constructors;
    }
  }

  return {};
}

std::vector<FunctionDecl *> ClassLinks::ownMethods(const ClassDecl *cls) {
  std::vector<FunctionDecl *> methods;
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->isStatic()) {
      methods.push_back(fn);
    }
  }
  for (const ClassField &f : cls->fields) {
    if (f.method()) {
      methods.push_back(f.method());
    }
  }

  return methods;
}

bool ClassLinks::overridesTransitively(const FunctionDecl *fn, const FunctionDecl *base) {
  for (const FunctionDecl *p = fn; p; p = p->overrides()) {
    if (p == base) {
      return true;
    }
  }

  return false;
}

FunctionDecl *ClassLinks::soleImplementation(const ClassDecl *cls, FunctionDecl *variant) {
  if (variant->openToLate()) {
    return nullptr;
  }
  if (!variant->overridden() && !variant->isAbstract()) {
    return variant;
  }
  if (variant->generic && variant->overridden()) {
    return nullptr;
  }
  FunctionDecl *only = variant->isAbstract() ? nullptr : variant;
  unsigned count = only ? 1 : 0;
  std::vector<ClassDecl *> below =
      cls != nullptr ? ClassTree::descendants(cls) : std::vector<ClassDecl *>();
  for (const ClassDecl *child : below) {
    for (FunctionDecl *fn : ClassLinks::ownMethods(child)) {
      if (!fn->isAbstract() && fn != variant && ClassLinks::overridesTransitively(fn, variant)) {
        only = fn;
        count += child->isEmitted() ? 1 : 2;
      }
    }
  }

  return count == 1 ? only : nullptr;
}

FunctionDecl *ClassLinks::firstImplementation(const ClassDecl *cls, const FunctionDecl *fn) {
  for (const ClassDecl *child : ClassTree::descendants(cls)) {
    for (FunctionDecl *impl : ClassLinks::ownMethods(child)) {
      if (!impl->isAbstract() && ClassLinks::overridesTransitively(impl, fn)) {
        return impl;
      }
    }
  }

  return nullptr;
}

const Type *ClassLinks::commonClass(const Type *a, const Type *b) {
  const ClassDecl *x = a->classDecl();
  const ClassDecl *y = b->classDecl();
  if (!x || !y) {
    return nullptr;
  }
  for (const ClassDecl *c = x; c; c = c->parent) {
    if (y->derivesFrom(c)) {
      return c->type;
    }
  }

  return nullptr;
}

void ClassLinks::linkClasses() {
  std::vector<ClassDecl *> order = ClassTree::treeOrder(this->state.getClassStore().getClasses());

  for (ClassDecl *cls : order) {
    this->linkOverrides(cls);
  }
  this->numberClasses();

  for (ClassDecl *cls : order) {
    this->buildVtable(cls);
  }
  this->state.getClassStore().markClassesLinked();

  llvm::SmallPtrSet<const FunctionDecl *, 4> sources;
  for (ClassDecl *cls : order) {
    if (cls->generic()) {
      continue;
    }
    for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
      if (!fn->sym || ClassLinks::declaresReturn(fn) || sources.contains(fn)) {
        continue;
      }
      if (fn->isAbstract() && !fn->overrides()) {
        FunctionDecl *source = ClassLinks::firstImplementation(cls, fn);
        fn->sym->setReturnType(nullptr);
        this->state.getClassStore().rememberReturnSource(fn, source);
        if (source) {
          sources.insert(source);
        }
        continue;
      }
      if (fn->overrides()) {
        FunctionDecl *base = fn->overrides();
        if (ClassLinks::declaresReturn(base) && base->sym && base->sym->hasReturnType()) {
          fn->sym->setReturnType(base->sym->getKnownReturnType());
        } else {
          fn->sym->setReturnType(nullptr);
          this->state.getClassStore().rememberInheritedReturn(fn, base);
        }
        continue;
      }
      std::vector<const Type *> interfaces;
      for (const ClassDecl *c = cls; c; c = c->parent) {
        ClassLinks::interfacesOf(c, interfaces);
      }
      for (const Type *iface : interfaces) {
        const Field *member = iface->field(fn->name);
        if (member && ClassLinks::takesParams(member->type, fn->sym)) {
          fn->sym->setReturnType(member->type->ret());
          break;
        }
        if (const Overload *overload = iface->overload(fn->name)) {
          for (const Type *signature : overload->signatures) {
            if (ClassLinks::takesParams(signature, fn->sym)) {
              fn->sym->setReturnType(signature->ret());
            }
          }
        }
      }
    }
  }
}

void ClassLinks::numberClasses() {
  std::vector<ClassDecl *> order = ClassTree::treeOrder(this->state.getClassStore().getClasses());
  unsigned next = 0;
  for (ClassDecl *cls : order) {
    cls->id = ++next;
  }
  for (ClassDecl *cls : llvm::reverse(order)) {
    cls->lastId = cls->id;
    for (const ClassDecl *child : cls->children) {
      cls->lastId = std::max(cls->lastId, child->lastId);
    }
  }
  for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
    if (cls->literal()) {
      cls->id = cls->lastId = ++next;
    }
  }
  this->state.getClassStore().rememberClassCount(next);
}

void ClassLinks::buildVtable(ClassDecl *cls) {
  cls->vtable = cls->parent ? cls->parent->vtable : std::vector<FunctionDecl *>();
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    Overriding *overriding = fn->overriding();
    if (fn->overrides()) {
      overriding->slot = fn->overrides()->slot();
      if (fn->slot() >= 0 && size_t(fn->slot()) < cls->vtable.size()) {
        cls->vtable[size_t(fn->slot())] = fn->isAbstract() ? nullptr : fn;
      }
      continue;
    }
    if (fn->overridden() || fn->isAbstract()) {
      overriding->slot = int(cls->vtable.size());
      cls->vtable.push_back(fn->isAbstract() ? nullptr : fn);
    }
  }
}

ClassLinks::ResultMethodNotField ClassLinks::notAField(const ClassDecl *cls,
                                                       const FunctionDecl *fn) {
  const ClassDecl *owner = nullptr;
  const ClassField *field =
      cls->parent ? ClassBases::findField(cls->parent, fn->name, &owner) : nullptr;
  if (field && !field->method()) {
    std::string message = ErrorTextNotAField::fieldOf(fn->name, owner->name);

    return err(ClassLinksNotAFieldError{
        .code = ClassLinksNotAFieldErrorCode::FIELD_OF_PARENT,
        .message = message,
    });
  }

  return ok(fn);
}

ClassLinks::ResultOverrideTarget
ClassLinks::overrideTarget(const ClassDecl *cls, const FunctionDecl *fn,
                           llvm::ArrayRef<FunctionDecl *> inherited) {
  if (fn->isOverride() && !cls->parent) {
    std::string message = ErrorTextOverrideTarget::noParent(fn->name, cls->name);

    return err(ClassLinksOverrideTargetError{
        .code = ClassLinksOverrideTargetErrorCode::NO_PARENT,
        .message = message,
    });
  }
  if (fn->isOverride() && inherited.empty()) {
    std::string message = ErrorTextOverrideTarget::noMethod(fn->name, cls->parent->name);

    return err(ClassLinksOverrideTargetError{
        .code = ClassLinksOverrideTargetErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }
  if (fn->isOverride()) {
    std::string list = ErrorTextVariant::list(inherited);
    std::string message =
        ErrorTextOverrideTarget::noParams(fn->name, cls->parent->name, inherited.size() == 1, list);

    return err(ClassLinksOverrideTargetError{
        .code = ClassLinksOverrideTargetErrorCode::NO_MATCHING_PARAMS,
        .message = message,
    });
  }

  return ok(fn);
}

ClassLinks::ResultBaseIsNotArrow ClassLinks::arrowOverride(const FunctionDecl *fn,
                                                           const FunctionDecl *base) {
  if (ClassLinks::isArrowMethod(base) && !ClassLinks::isArrowMethod(fn)) {
    std::string message = ErrorTextArrowOverride::arrow(fn->name, base->cls()->name);

    return err(ClassLinksArrowOverrideError{
        .code = ClassLinksArrowOverrideErrorCode::ARROW_IN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

ClassLinks::ResultBaseNotPrivate ClassLinks::baseVisible(const FunctionDecl *fn,
                                                         const FunctionDecl *base) {
  if (base->access() == Access::Private) {
    std::string message = ErrorTextBaseVisible::hidden(fn->name, base->cls()->name);

    return err(ClassLinksBaseVisibleError{
        .code = ClassLinksBaseVisibleErrorCode::PRIVATE_IN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

ClassLinks::ResultAccessNotLower ClassLinks::accessKept(const FunctionDecl *fn,
                                                        const FunctionDecl *base) {
  if (fn->access() > base->access()) {
    llvm::StringRef baseAccess = ClassLinks::accessName(base->access());
    llvm::StringRef access = ClassLinks::accessName(fn->access());
    std::string message =
        ErrorTextAccessKept::narrower(fn->name, baseAccess, base->cls()->name, access);

    return err(ClassLinksAccessKeptError{
        .code = ClassLinksAccessKeptErrorCode::NARROWER_ACCESS,
        .message = message,
    });
  }

  return ok(fn);
}

ClassLinks::ResultBodyIsRetained ClassLinks::bodyKept(const ClassDecl *cls, const FunctionDecl *fn,
                                                      const FunctionDecl *base) {
  if (fn->isAbstract() && !base->isAbstract()) {
    std::string message = ErrorTextBodyKept::again(fn->name, base->cls()->name, cls->name);

    return err(ClassLinksBodyKeptError{
        .code = ClassLinksBodyKeptErrorCode::ABSTRACT_AGAIN,
        .message = message,
    });
  }

  return ok(fn);
}

ClassLinks::ResultSameTypeParams ClassLinks::sameTypeArgs(const ClassDecl *cls,
                                                          const FunctionDecl *fn,
                                                          const FunctionDecl *base) const {
  if (this->state.getClassStore().hasLinkedClasses() && !base->overridden() && base->slot() < 0) {
    std::string message = ErrorTextSameTypeArgs::typeArgs(
        fn->name, cls->type->name(), base->cls()->type->name(), base->name, base->cls()->name);

    return err(ClassLinksSameTypeArgsError{
        .code = ClassLinksSameTypeArgsErrorCode::OVERRIDE_WITH_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(fn);
}

void ClassLinks::linkOverrides(ClassDecl *cls) {
  bool muted = this->state.getDiagnostics().setMuted(false);
  this->state.getDiagnostics().setMuted(muted || cls->generic() != nullptr);
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    if (fn->sym != nullptr) {
      this->linkOverride(cls, fn);
    }
  }
  this->state.getDiagnostics().setMuted(muted);
}

void ClassLinks::linkOverride(ClassDecl *cls, FunctionDecl *fn) {
  std::vector<FunctionDecl *> inherited;
  if (cls->parent != nullptr) {
    inherited = ClassDeclarer::methodsNamed(cls->parent, fn->name, false);
  }
  FunctionDecl *base = ClassLinks::sameParamsBase(inherited, fn);
  ResultMethodNotField method = ClassLinks::notAField(cls, fn);
  if (method.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), method.error().message);
    return;
  }
  if (base == nullptr) {
    this->walkNewMethod(cls, fn, inherited);
    return;
  }
  if (!this->overrideFits(cls, fn, base)) {
    return;
  }
  fn->overriding()->overrides = base;
  this->walkOverrideChain(cls, fn, base);
}

FunctionDecl *ClassLinks::sameParamsBase(llvm::ArrayRef<FunctionDecl *> inherited,
                                         const FunctionDecl *fn) {
  FunctionDecl *base = nullptr;
  for (FunctionDecl *v : inherited) {
    if (v->sym != nullptr && ClassLinks::sameParams(v->sym, fn->sym)) {
      base = v;
    }
  }

  return base;
}

void ClassLinks::walkNewMethod(const ClassDecl *cls, FunctionDecl *fn,
                               llvm::ArrayRef<FunctionDecl *> inherited) {
  ResultOverrideTarget target = ClassLinks::overrideTarget(cls, fn, inherited);
  if (target.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), target.error().message);
  }
  if (!inherited.empty() && this->chunkTypes->isStreamHook(fn, true)) {
    ChunkTypes::ResultHookParamsMatch params = this->chunkTypes->hookParams(fn, inherited);
    if (params.isErr() && params.error().code == ChunkTypesHookParamsErrorCode::OTHER_PARAMETERS) {
      this->state.getDiagnostics().error(fn->loc(), params.error().message);
    }
    return;
  }
  for (FunctionDecl *v : inherited) {
    this->classDeclarer->checkVariantPair(v, fn);
  }
}

bool ClassLinks::overrideFits(const ClassDecl *cls, const FunctionDecl *fn,
                              const FunctionDecl *base) {
  ResultBaseIsNotArrow notArrow = ClassLinks::arrowOverride(fn, base);
  if (notArrow.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), notArrow.error().message);
    return false;
  }
  ResultBaseNotPrivate visible = ClassLinks::baseVisible(fn, base);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), visible.error().message);
    return false;
  }
  ResultAccessNotLower access = ClassLinks::accessKept(fn, base);
  if (access.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), access.error().message);
  }
  ResultBodyIsRetained body = ClassLinks::bodyKept(cls, fn, base);
  if (body.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), body.error().message);
  }

  return true;
}

void ClassLinks::walkOverrideChain(const ClassDecl *cls, const FunctionDecl *fn,
                                   FunctionDecl *base) {
  for (FunctionDecl *p = base; p != nullptr; p = p->overrides()) {
    ResultSameTypeParams same = this->sameTypeArgs(cls, fn, p);
    if (same.isErr()) {
      bool outer = this->state.getDiagnostics().setMuted(false);
      this->state.getDiagnostics().error(fn->loc(), same.error().message);
      this->state.getDiagnostics().setMuted(outer);
    }
    this->genericClassInstantiator->markOverridden(p);
  }
}

const Type *ClassLinks::inheritedReturnType(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  if (!decl) {
    return nullptr;
  }
  ClassStore &classes = this->state.getClassStore();
  if (classes.hasReturnSource(decl)) {
    FunctionDecl *source = classes.findReturnSource(decl);
    return source ? source->sym->getReturnType() : Type::voidTy();
  }
  if (FunctionDecl *base = classes.findInheritedReturn(decl)) {
    return base->sym->getReturnType();
  }

  return nullptr;
}

void ClassLinks::noteInheritedReturn(const FunctionDecl *fn) {
  if (!fn || !fn->cls() || fn->returnType || !fn->sym || !fn->sym->hasReturnType()) {
    return;
  }
  std::string type = fn->sym->getKnownReturnType()->name().str();
  if (const FunctionDecl *base = fn->overrides()) {
    std::string returns = ErrorTextInheritedReturn::returns(fn->name, base->cls()->name, type);
    this->state.getDiagnostics().note(base->loc(), returns);
  }
}

}
