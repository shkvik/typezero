#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

bool sameParams(const FuncSymbol *a, const FuncSymbol *b) {
  return a->params == b->params && a->minArgs == b->minArgs && a->hasRest == b->hasRest;
}

bool takesParams(const Type *fnType, const FuncSymbol *sym) {
  return fnType->isFunction() && fnType->params() == llvm::ArrayRef<const Type *>(sym->params) &&
         fnType->minArgs() == sym->minArgs && fnType->hasRest() == sym->hasRest;
}

const char *accessName(Access access) {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
}

bool isArrowMethod(const FunctionDecl *fn) {
  if (!fn->cls) {
    return false;
  }
  for (const ClassField &f : fn->cls->fields) {
    if (f.method == fn) {
      return true;
    }
  }

  return false;
}

bool declaresReturn(const FunctionDecl *fn) {
  if (fn->returnType) {
    return true;
  }
  if (!fn->cls) {
    return false;
  }
  for (const ClassField &f : fn->cls->fields) {
    if (f.method == fn && f.type) {
      return true;
    }
  }

  return false;
}

void interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out) {
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

}

ClassLinks::ClassLinks(Checker &checker) : checker_(checker) {}

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
    if (!fn->isStatic) {
      methods.push_back(fn);
    }
  }
  for (const ClassField &f : cls->fields) {
    if (f.method) {
      methods.push_back(f.method);
    }
  }

  return methods;
}

bool ClassLinks::overridesTransitively(const FunctionDecl *fn, const FunctionDecl *base) {
  for (const FunctionDecl *p = fn; p; p = p->overrides) {
    if (p == base) {
      return true;
    }
  }

  return false;
}

FunctionDecl *ClassLinks::soleImplementation(const ClassDecl *cls, FunctionDecl *variant) {
  if (variant->openToLate) {
    return nullptr;
  }
  if (!variant->overridden && !variant->isAbstract) {
    return variant;
  }
  if (variant->generic && variant->overridden) {
    return nullptr;
  }
  FunctionDecl *only = variant->isAbstract ? nullptr : variant;
  unsigned count = only ? 1 : 0;
  std::function<void(const ClassDecl *)> walk = [&](const ClassDecl *c) {
    for (const ClassDecl *child : c->children) {
      for (FunctionDecl *fn : ownMethods(child)) {
        if (!fn->isAbstract && fn != variant && overridesTransitively(fn, variant)) {
          only = fn;
          count += child->isEmitted() ? 1 : 2;
        }
      }
      walk(child);
    }
  };
  if (cls) {
    walk(cls);
  }

  return count == 1 ? only : nullptr;
}

const Type *ClassLinks::commonClass(const Type *a, const Type *b) {
  const ClassDecl *x = a->classDecl(), *y = b->classDecl();
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
  std::vector<ClassDecl *> order;
  std::function<void(ClassDecl *)> visit = [&](ClassDecl *cls) {
    order.push_back(cls);
    for (ClassDecl *child : cls->children) {
      visit(child);
    }
  };
  for (ClassDecl *cls : checker_.classes_) {
    if (!cls->parent && !cls->literal) {
      visit(cls);
    }
  }

  for (ClassDecl *cls : order) {
    linkOverrides(cls);
  }
  numberClasses();

  for (ClassDecl *cls : order) {
    buildVtable(cls);
  }
  checker_.classesLinked_ = true;

  llvm::SmallPtrSet<const FunctionDecl *, 4> sources;
  for (ClassDecl *cls : order) {
    if (cls->generic) {
      continue;
    }
    for (FunctionDecl *fn : ownMethods(cls)) {
      if (!fn->sym || declaresReturn(fn) || sources.contains(fn)) {
        continue;
      }
      if (fn->isAbstract && !fn->overrides) {
        FunctionDecl *source = nullptr;
        std::function<void(const ClassDecl *)> find = [&](const ClassDecl *c) {
          for (const ClassDecl *child : c->children) {
            for (FunctionDecl *impl : ownMethods(child)) {
              if (!source && !impl->isAbstract && overridesTransitively(impl, fn)) {
                source = impl;
              }
            }
            find(child);
          }
        };
        find(cls);
        fn->sym->ret = nullptr;
        checker_.returnSources_[fn] = source;
        if (source) {
          sources.insert(source);
        }
        continue;
      }
      if (fn->overrides) {
        FunctionDecl *base = fn->overrides;
        if (declaresReturn(base) && base->sym && base->sym->ret) {
          fn->sym->ret = base->sym->ret;
        } else {
          fn->sym->ret = nullptr;
          checker_.inheritsReturn_[fn] = base;
        }
        continue;
      }
      std::vector<const Type *> interfaces;
      for (const ClassDecl *c = cls; c; c = c->parent) {
        interfacesOf(c, interfaces);
      }
      for (const Type *iface : interfaces) {
        const Field *member = iface->field(fn->name);
        if (member && takesParams(member->type, fn->sym)) {
          fn->sym->ret = member->type->ret();
          break;
        }
        if (const Overload *overload = iface->overload(fn->name)) {
          for (const Type *signature : overload->signatures) {
            if (takesParams(signature, fn->sym)) {
              fn->sym->ret = signature->ret();
            }
          }
        }
      }
    }
  }
}

void ClassLinks::numberClasses() {
  std::vector<ClassDecl *> order;
  std::function<void(ClassDecl *)> visit = [&](ClassDecl *cls) {
    order.push_back(cls);
    for (ClassDecl *child : cls->children) {
      visit(child);
    }
  };
  for (ClassDecl *cls : checker_.classes_) {
    if (!cls->parent && !cls->literal) {
      visit(cls);
    }
  }
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
  for (ClassDecl *cls : checker_.classes_) {
    if (cls->literal) {
      cls->id = cls->lastId = ++next;
    }
  }
  checker_.classCount_ = next;
}

void ClassLinks::buildVtable(ClassDecl *cls) {
  cls->vtable = cls->parent ? cls->parent->vtable : std::vector<FunctionDecl *>();
  for (FunctionDecl *fn : ownMethods(cls)) {
    if (fn->overrides) {
      fn->slot = fn->overrides->slot;
      if (fn->slot >= 0 && size_t(fn->slot) < cls->vtable.size()) {
        cls->vtable[size_t(fn->slot)] = fn->isAbstract ? nullptr : fn;
      }
      continue;
    }
    if (fn->overridden || fn->isAbstract) {
      fn->slot = int(cls->vtable.size());
      cls->vtable.push_back(fn->isAbstract ? nullptr : fn);
    }
  }
}

void ClassLinks::linkOverrides(ClassDecl *cls) {
  bool muted = checker_.diags_.setMuted(false);
  checker_.diags_.setMuted(muted || cls->generic != nullptr);
  for (FunctionDecl *fn : ownMethods(cls)) {
    if (!fn->sym) {
      continue;
    }
    FunctionDecl *base = nullptr;
    std::vector<FunctionDecl *> inherited;
    if (cls->parent) {
      inherited = ClassDeclarer::methodsNamed(cls->parent, fn->name, false);
      for (FunctionDecl *v : inherited) {
        if (v->sym && sameParams(v->sym, fn->sym)) {
          base = v;
        }
      }
    }
    const ClassDecl *owner = nullptr;
    const ClassField *field =
        cls->parent ? ClassBases::findField(cls->parent, fn->name, &owner) : nullptr;
    if (field && !field->method) {
      checker_.diags_.error(fn->loc(),
                            "'" + fn->name + "' is a field of '" + owner->name +
                                "', so a class that extends it cannot have a method of that "
                                "name");
      continue;
    }
    if (!base) {
      if (fn->isOverride) {
        if (!cls->parent) {
          checker_.diags_.error(fn->loc(), "'override' says that '" + fn->name +
                                               "' replaces a method of the class that '" +
                                               cls->name + "' extends, and it extends none");
        } else if (inherited.empty()) {
          checker_.diags_.error(fn->loc(), "'" + fn->name + "' is marked 'override', but '" +
                                               cls->parent->name + "' has no method '" + fn->name +
                                               "'");
        } else {
          checker_.diags_.error(
              fn->loc(), "'" + fn->name + "' is marked 'override', but no method of '" +
                             cls->parent->name + "' has its parameters: there " +
                             (inherited.size() == 1 ? "is " : "are ") + variantList(inherited) +
                             "; an override takes the same parameters");
        }
      }
      if (!inherited.empty() && checker_.chunkTypes().isStreamHook(fn, true)) {
        checker_.chunkTypes().reportStreamHook(fn, inherited);
        continue;
      }
      for (FunctionDecl *v : inherited) {
        checker_.classDeclarer().checkVariantPair(v, fn);
      }
      continue;
    }
    if (isArrowMethod(base) && !isArrowMethod(fn)) {
      checker_.diags_.error(fn->loc(),
                            "'" + fn->name + "' is an arrow in a field of '" + base->cls->name +
                                "', which each object holds itself in JavaScript, so a method of "
                                "a class that extends it would never run; override it with a "
                                "field too: '" +
                                fn->name + " = (...) => ...'");
      continue;
    }
    if (base->access == Access::Private) {
      checker_.diags_.error(fn->loc(), "'" + fn->name + "' is private in class '" +
                                           base->cls->name +
                                           "', so a class that extends it cannot override it");
      continue;
    }
    if (fn->access > base->access) {
      checker_.diags_.error(fn->loc(), "'" + fn->name + "' is " + accessName(base->access) +
                                           " in '" + base->cls->name +
                                           "', so its override cannot be " +
                                           accessName(fn->access));
    }
    if (fn->isAbstract && !base->isAbstract) {
      checker_.diags_.error(fn->loc(), "'" + fn->name + "' has a body in '" + base->cls->name +
                                           "', so it cannot be abstract again in '" + cls->name +
                                           "'");
    }
    fn->overrides = base;
    for (FunctionDecl *p = base; p; p = p->overrides) {
      if (checker_.classesLinked_ && !p->overridden && p->slot < 0) {
        bool outer = checker_.diags_.setMuted(false);
        checker_.diags_.error(fn->loc(),
                              "'" + fn->name + "' of '" + cls->type->name() + "' overrides '" +
                                  p->cls->type->name() + "." + p->name +
                                  "' only with these type arguments, which is not supported; give "
                                  "the method the same parameters in '" +
                                  p->cls->name + "'");
        checker_.diags_.setMuted(outer);
      }
      checker_.genericClassInstantiator().markOverridden(p);
    }
  }
  checker_.diags_.setMuted(muted);
}

const Type *ClassLinks::inheritedReturnType(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  if (!decl) {
    return nullptr;
  }
  if (auto found = checker_.returnSources_.find(decl); found != checker_.returnSources_.end()) {
    return found->second ? checker_.returnInferrer().returnType(found->second->sym)
                         : Type::voidTy();
  }
  if (auto found = checker_.inheritsReturn_.find(decl); found != checker_.inheritsReturn_.end()) {
    return checker_.returnInferrer().returnType(found->second->sym);
  }

  return nullptr;
}

void ClassLinks::noteInheritedReturn(const FunctionDecl *fn) {
  if (!fn || !fn->cls || fn->returnType || !fn->sym || !fn->sym->ret) {
    return;
  }
  std::string type = fn->sym->ret->name().str();
  if (const FunctionDecl *base = fn->overrides) {
    checker_.diags_.note(base->loc(), "'" + fn->name + "' overrides '" + base->cls->name + "." +
                                          fn->name + "', so it returns '" + type +
                                          "' too; to return another type, "
                                          "declare it in '" +
                                          base->cls->name + "'");
  }
}

}
