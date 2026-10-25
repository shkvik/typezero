// Inheritance and interfaces (docs/objects.md, sections 1, 2 and 6;
// docs/overloads.md, section 4).
//
// A class extends at most one class and implements interfaces; an interface
// extends interfaces. These declared relations are the only subtypes: an
// object fits a type up its chain, and no other type that happens to have the
// same fields. An object has the fields of the class it extends first, at the
// same offsets, so a field reads the same way through either type.
//
// A method of a subclass with the name and the parameters of a method up the
// chain overrides it, with `override` or without, as in TypeScript; a method
// with other parameters is one more variant of the name. A call goes through
// the method table of the object's class only if some class the object may
// belong to overrides the method; otherwise it is direct, as it is without
// inheritance. Classes are numbered so that those down the chain of a class
// follow it: `instanceof` compares the number with a range.
//
// A subclass or an interface that extends another may narrow a field to
// objects of a type down the chain (`pet: Dog` for `pet: Animal`) or make it
// read-only, and an array of Dog is an array of Animal. Code that writes
// through the wider type what the narrower one does not allow is found by the
// check of the whole program (sema_widen.cpp).

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The classes of errors built into JavaScript; tz has Error, TypeError,
// RangeError and SyntaxError of them (see sema_error.cpp).
bool isErrorClass(llvm::StringRef name) {
  static const char *const names[] = {"Error",       "TypeError",      "RangeError",
                                      "SyntaxError", "ReferenceError", "EvalError",
                                      "URIError",    "AggregateError"};
  return llvm::is_contained(names, name);
}

bool sameParams(const FuncSymbol *a, const FuncSymbol *b) {
  return a->params == b->params && a->minArgs == b->minArgs && a->hasRest == b->hasRest;
}

// Whether a function type takes the parameters of `sym`.
bool takesParams(const Type *fnType, const FuncSymbol *sym) {
  return fnType->isFunction() && fnType->params() == llvm::ArrayRef<const Type *>(sym->params) &&
         fnType->minArgs() == sym->minArgs && fnType->hasRest() == sym->hasRest;
}

const char *accessName(Access access) {
  return access == Access::Public      ? "public"
         : access == Access::Protected ? "protected"
                                       : "private";
}

// An arrow in a field of a class that is compiled as a method.
bool isArrowMethod(const FunctionDecl *fn) {
  if (!fn->cls)
    return false;
  for (const ClassField &f : fn->cls->fields)
    if (f.method == fn)
      return true;
  return false;
}

// Whether the return type of a method is written: in its declaration, or by
// the type of the field an arrow method is in.
bool declaresReturn(const FunctionDecl *fn) {
  if (fn->returnType)
    return true;
  if (!fn->cls)
    return false;
  for (const ClassField &f : fn->cls->fields)
    if (f.method == fn && f.type)
      return true;
  return false;
}

// The interfaces a class implements, and those they extend, nearest first.
void interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out) {
  std::vector<const Type *> queue(cls->implemented.begin(), cls->implemented.end());
  for (size_t i = 0; i < queue.size(); ++i) {
    if (llvm::is_contained(out, queue[i]))
      continue;
    out.push_back(queue[i]);
    for (const Type *base : queue[i]->bases())
      queue.push_back(base);
  }
}

} // namespace

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

void Sema::addBase(const Type *type, const Type *base) {
  const_cast<Type *>(type)->addBase(base);
  // CodeGen lays out the types of the code it emits: not those of a copy
  // checked to infer a type, nor of a generic body (its instances have their
  // own).
  bool emitted = !trial_ && llvm::none_of(functions_, [](const FunctionDecl *fn) {
    return !fn->typeParams.empty() && !fn->isInstance;
  });
  if (file_ && emitted && !llvm::is_contained(file_->subtypes, type))
    file_->subtypes.push_back(type);
}

void Sema::resolveClassBases(ClassDecl *cls) {
  cls->resolvingBase = true;
  if (cls->base) {
    const TypeRef &ref = *cls->base;
    bool named = ref.kind == TypeRef::Kind::Name || ref.kind == TypeRef::Kind::Generic;
    Stmt *found = named ? lookupType(ref.name) : nullptr;
    auto *parent = llvm::dyn_cast_or_null<ClassDecl>(found);
    bool instance = false;
    // `extends EventEmitter` without a map: the one found from the emits of
    // the program (sema_events.cpp).
    if (parent && parent->library && parent->name == "EventEmitter" &&
        ref.kind == TypeRef::Kind::Name) {
      const Type *events = inferredEventMap("class " + placeOf(cls->loc()) + " " + cls->name.str());
      parent = events ? instantiateClass(parent, {events}, ref.loc) : nullptr;
      instance = true;
      if (!events)
        diags_.error(ref.loc, "'EventEmitter' needs its event map here, e.g. 'extends "
                              "EventEmitter<{ data: [chunk: string]; end: [] }>'");
      if (!parent)
        found = nullptr;
    } else if (parent && (parent->isGeneric() || ref.kind == TypeRef::Kind::Generic)) {
      const Type *type = resolveType(ref, /*allowVoid=*/false);
      parent = classOf(type);
      instance = true;
      if (!parent)
        found = nullptr;
    }
    if (!named) {
      diags_.error(ref.loc, "a class can extend only a class, by its name");
    } else if (instance && !parent) {
      // An error in the type arguments, reported.
    } else if (parent && (parent == cls || parent->resolvingBase)) {
      diags_.error(ref.loc,
                   "class '" + cls->name + "' cannot extend itself" +
                       (parent == cls ? std::string() : " through '" + parent->name.str() + "'"));
    } else if (isRuntimeClass(parent)) {
      diags_.error(ref.loc, "class '" + cls->name + "' cannot extend '" + parent->name +
                                "': only the functions of " +
                                (parent == execSyncErrorClass_ ? "node:child_process" : "node:fs") +
                                " make its objects" +
                                (derivesFromError(parent) ? "; extend Error" : ""));
    } else if (parent && parent->baseExpr && !parent->resolved) {
      // Its fields are known only where its declaration runs.
      diags_.error(ref.loc, "class '" + cls->name + "' cannot extend '" + parent->name +
                                "', whose base is a value, made where its declaration runs; "
                                "write it as an expression there: 'const " + cls->name +
                                " = class extends " + parent->name + " { ... }'");
    } else if (parent) {
      // In its own file, where the names it uses mean what they mean there,
      // and without the type parameters of this class.
      std::vector<llvm::StringMap<Stmt *>> outer = typeScopes_;
      typeScopes_.resize(1);
      if (Module *home = declModules_.lookup(parent);
          home && home != module_ && scopes_.size() == 1) {
        Module *here = module_;
        useModule(home);
        resolveClass(parent);
        useModule(here);
      } else {
        resolveClass(parent);
      }
      typeScopes_ = std::move(outer);
      cls->parent = parent;
      parent->children.push_back(cls);
      addBase(cls->type, parent->type);
    } else if (!found && isErrorClass(ref.name)) {
      diags_.error(ref.loc, "'" + ref.name +
                                "' is not built in; extend Error, or one of its kinds that are: "
                                "TypeError, RangeError, SyntaxError");
    } else if (found) {
      std::string message =
          "'" + ref.name.str() + "' is not a class, and a class can extend only a class";
      if (isa<TypeAliasDecl>(found))
        message += "; to say that '" + cls->name.str() +
                   "' has what an interface lists, write "
                   "'implements " +
                   ref.name.str() + "'";
      diags_.error(ref.loc, message);
    } else {
      diags_.error(ref.loc, "cannot find class '" + ref.name + "'");
    }
  }
  for (const TypeRef &ref : cls->interfaces) {
    const Type *type = resolveType(ref, /*allowVoid=*/false);
    if (type->isError())
      continue;
    if (type->isClass())
      diags_.error(ref.loc, "'" + type->name() +
                                "' is a class, and a class implements only "
                                "interfaces" +
                                (cls->base ? std::string()
                                           : "; to take its fields and methods, write 'extends " +
                                                 type->name().str() + "'"));
    else if (!type->isObject() || type->isTuple() || type->isAnyObject() || type->isIncomplete())
      diags_.error(ref.loc, "'" + type->name() +
                                "' is not an interface; a class implements object "
                                "types, such as 'interface Named { name: string }'");
    else if (!llvm::is_contained(cls->implemented, type)) {
      cls->implemented.push_back(type);
      addBase(cls->type, type);
    }
  }
  cls->resolvingBase = false;

  // A field declared up the chain already: this declaration gives it another
  // initial value, and its type must stay (checked in completeClass).
  for (ClassField &f : cls->fields) {
    if (f.isStatic)
      continue;
    const ClassDecl *owner = nullptr;
    const ClassField *inherited = cls->parent ? findField(cls->parent, f.name, &owner) : nullptr;
    // An arrow in a field that is a method up the chain: this one overrides it
    // (see linkOverrides).
    if (inherited && inherited->method)
      continue;
    if (!inherited) {
      if (f.isOverride)
        diags_.error(f.loc, cls->parent ? "'" + f.name + "' is marked 'override', but '" +
                                              cls->parent->name + "' has no field '" + f.name + "'"
                                        : "'override' says that '" + f.name +
                                              "' is a field of the class that '" + cls->name +
                                              "' extends, and it extends none");
      continue;
    }
    if (inherited->access == Access::Private) {
      diags_.error(f.loc, "'" + f.name + "' is private in class '" + owner->name +
                              "', so a class that extends it cannot declare it again");
      continue;
    }
    if (f.access > inherited->access)
      diags_.error(f.loc, "'" + f.name + "' is " + accessName(inherited->access) + " in '" +
                              owner->name + "', so it cannot be " + accessName(f.access) + " in '" +
                              cls->name + "'");
    // Without a value, JavaScript defines the field again as undefined
    // once the constructor up the chain has set it.
    if (!f.init && !f.parameter)
      diags_.error(f.loc, "'" + f.name + "' is a field of '" + owner->name +
                              "' already; declared again without a value, JavaScript would set "
                              "it to undefined after the constructor of '" + owner->name +
                              "' set it: drop the declaration, or give it a value");
    f.redeclared = true;
  }
}

std::vector<Field> Sema::interfaceFields(TypeAliasDecl *alias, std::vector<const Type *> &bases,
                                         std::vector<Overload> &overloads) {
  std::vector<Field> fields;
  std::vector<std::string> origins; // the interface each field comes from
  for (const TypeRef &ref : alias->bases) {
    Stmt *found = ref.kind == TypeRef::Kind::Name ? lookupType(ref.name) : nullptr;
    if (auto *other = llvm::dyn_cast_or_null<TypeAliasDecl>(found);
        other && (other == alias || other->resolving)) {
      diags_.error(ref.loc,
                   "interface '" + alias->name + "' cannot extend itself" +
                       (other == alias ? std::string() : " through '" + other->name.str() + "'"));
      continue;
    }
    const Type *base = resolveType(ref, /*allowVoid=*/false);
    if (base->isError())
      continue;
    if (base->isClass()) {
      diags_.error(ref.loc, "'" + base->name() +
                                "' is a class, and an interface extends only "
                                "interfaces");
      continue;
    }
    if (!base->isObject() || base->isTuple() || base->isIncomplete() || base->isAnyObject()) {
      diags_.error(ref.loc, "'" + base->name() +
                                "' is not an interface; an interface extends "
                                "object types, such as 'interface Named { name: string }'");
      continue;
    }
    if (llvm::is_contained(bases, base))
      continue;
    bases.push_back(base);
    for (const Overload &o : base->overloads()) {
      auto it = llvm::find_if(overloads, [&](const Overload &p) { return p.name == o.name; });
      if (it == overloads.end())
        overloads.push_back(o);
      else if (it->signatures != o.signatures)
        diags_.error(ref.loc, "interface '" + alias->name + "' gets method '" + o.name +
                                  "' from two interfaces with different signatures");
    }
    // By the name written: one with the same fields is the same type, which
    // has the name of the first.
    std::string baseName = ref.name.str();
    for (const Field &f : base->fields()) {
      auto it = llvm::find_if(fields, [&](const Field &g) { return g.name == f.name; });
      if (it == fields.end()) {
        fields.push_back(f);
        origins.push_back(baseName);
      } else if (it->type != f.type) {
        diags_.error(ref.loc, "interface '" + alias->name + "' gets field '" + f.name + "' from '" +
                                  origins[size_t(it - fields.begin())] + "' as '" +
                                  it->type->name() + "' and from '" + baseName + "' as '" +
                                  f.type->name() + "'; a field has one type");
      } else {
        // Read-only only if both say so: code that sees the object as the
        // other one may change it.
        it->readonly = it->readonly && f.readonly;
      }
    }
  }
  const TypeRef &own = alias->type;
  std::vector<Overload> ownOverloads;
  std::vector<Field> ownFields = resolveFields(own, &ownOverloads);
  for (const Overload &o : ownOverloads) {
    bool clash = llvm::any_of(fields, [&](const Field &f) { return f.name == o.name; }) ||
                 llvm::any_of(overloads, [&](const Overload &p) { return p.name == o.name; });
    if (clash)
      diags_.error(alias->loc(), "interface '" + alias->name + "' declares method '" + o.name +
                                     "' again; an interface cannot change the members of one "
                                     "it extends");
    else
      overloads.push_back(o);
  }
  for (Field &f : ownFields) {
    if (llvm::any_of(overloads, [&](const Overload &o) { return o.name == f.name; })) {
      diags_.error(alias->loc(), "interface '" + alias->name + "' declares '" + f.name +
                                     "' again; an interface cannot change the members of one "
                                     "it extends");
      continue;
    }
    llvm::SMLoc loc = alias->loc();
    for (size_t i = 0; i < own.names.size(); ++i)
      if (own.names[i] == f.name)
        loc = own.locs[i];
    auto it = llvm::find_if(fields, [&](const Field &g) { return g.name == f.name; });
    if (it == fields.end()) {
      fields.push_back(f);
      origins.push_back(alias->name.str());
      continue;
    }
    const std::string &origin = origins[size_t(it - fields.begin())];
    // An interface may narrow a field of one it extends to a type of the same
    // references, or make it read-only; code that writes through the wider
    // one is found by the check of the whole program (sema_widen.cpp).
    if (it->type != f.type && !f.type->isError() && !it->type->isError() &&
        !sameReference(f.type, it->type)) {
      diags_.error(loc, "field '" + f.name + "' of '" + alias->name + "' is '" + f.type->name() +
                            "', and '" + origin + "' declares it as '" + it->type->name() +
                            "'; an interface can narrow a field of one it extends only to objects "
                            "of a type down the chain of its type, such as 'Dog' for 'Animal', "
                            "which lie in memory as they do");
      continue;
    }
    narrowedFields_ |= it->type != f.type || (f.readonly && !it->readonly);
    it->type = f.type;
    it->readonly = f.readonly;
  }
  return fields;
}

//===----------------------------------------------------------------------===//
// Methods up and down the chain
//===----------------------------------------------------------------------===//

const ClassField *Sema::findField(const ClassDecl *cls, llvm::StringRef name,
                                  const ClassDecl **owner) {
  for (const ClassDecl *c = cls; c; c = c->parent)
    if (const ClassField *f = c->field(name)) {
      if (owner)
        *owner = c;
      return f;
    }
  return nullptr;
}

std::vector<FunctionDecl *> Sema::constructorsOf(const ClassDecl *cls) {
  for (const ClassDecl *c = cls; c; c = c->parent)
    if (!c->constructors.empty())
      return c->constructors;
  return {};
}

std::vector<FunctionDecl *> Sema::ownMethods(const ClassDecl *cls) {
  std::vector<FunctionDecl *> methods;
  for (FunctionDecl *fn : cls->methods)
    if (!fn->isStatic)
      methods.push_back(fn);
  for (const ClassField &f : cls->fields)
    if (f.method)
      methods.push_back(f.method);
  return methods;
}

bool Sema::overridesTransitively(const FunctionDecl *fn, const FunctionDecl *base) {
  for (const FunctionDecl *p = fn; p; p = p->overrides)
    if (p == base)
      return true;
  return false;
}

FunctionDecl *Sema::soleImplementation(const ClassDecl *cls, FunctionDecl *variant) {
  // Classes made later, as code is checked, may override it.
  if (variant->openToLate)
    return nullptr;
  if (!variant->overridden && !variant->isAbstract)
    return variant;
  // A method of an instance of a generic class that some class overrides:
  // the instances that extend it may come later than this call.
  if (variant->generic && variant->overridden)
    return nullptr;
  FunctionDecl *only = variant->isAbstract ? nullptr : variant;
  unsigned count = only ? 1 : 0;
  std::function<void(const ClassDecl *)> walk = [&](const ClassDecl *c) {
    for (const ClassDecl *child : c->children) {
      for (FunctionDecl *fn : ownMethods(child))
        if (!fn->isAbstract && fn != variant && overridesTransitively(fn, variant)) {
          only = fn;
          // A generic class stands for its instances, which may be many.
          count += child->isEmitted() ? 1 : 2;
        }
      walk(child);
    }
  };
  if (cls)
    walk(cls);
  return count == 1 ? only : nullptr;
}

const Type *Sema::commonClass(const Type *a, const Type *b) {
  const ClassDecl *x = a->classDecl(), *y = b->classDecl();
  if (!x || !y)
    return nullptr;
  for (const ClassDecl *c = x; c; c = c->parent)
    if (y->derivesFrom(c))
      return c->type;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Linking the classes
//===----------------------------------------------------------------------===//

void Sema::linkClasses() {
  // Each class before those down its chain, in the order declared.
  std::vector<ClassDecl *> order;
  std::function<void(ClassDecl *)> visit = [&](ClassDecl *cls) {
    order.push_back(cls);
    for (ClassDecl *child : cls->children)
      visit(child);
  };
  for (ClassDecl *cls : classes_)
    if (!cls->parent && !cls->literal)
      visit(cls);

  for (ClassDecl *cls : order)
    linkOverrides(cls);
  numberClasses();

  // The method tables: a class has the slots of the one it extends, then one
  // for each method of its own that is abstract or that a class down the
  // chain overrides.
  for (ClassDecl *cls : order)
    buildVtable(cls);
  classesLinked_ = true;

  // The return types that methods take from what they override or
  // implement, when they declare none. An abstract method without one takes
  // that of its first implementation. A method of an instance of a generic
  // class has the type of the method of the generic class.
  llvm::SmallPtrSet<const FunctionDecl *, 4> sources;
  for (ClassDecl *cls : order) {
    if (cls->generic)
      continue;
    for (FunctionDecl *fn : ownMethods(cls)) {
      if (!fn->sym || declaresReturn(fn) || sources.contains(fn))
        continue;
      if (fn->isAbstract && !fn->overrides) {
        FunctionDecl *source = nullptr;
        std::function<void(const ClassDecl *)> find = [&](const ClassDecl *c) {
          for (const ClassDecl *child : c->children) {
            for (FunctionDecl *impl : ownMethods(child))
              if (!source && !impl->isAbstract && overridesTransitively(impl, fn))
                source = impl;
            find(child);
          }
        };
        find(cls);
        fn->sym->ret = nullptr;
        returnSources_[fn] = source;
        if (source)
          sources.insert(source);
        continue;
      }
      if (fn->overrides) {
        FunctionDecl *base = fn->overrides;
        if (declaresReturn(base) && base->sym && base->sym->ret) {
          fn->sym->ret = base->sym->ret;
        } else {
          fn->sym->ret = nullptr;
          inheritsReturn_[fn] = base;
        }
        continue;
      }
      // A method of an interface the class implements gives its type.
      std::vector<const Type *> interfaces;
      for (const ClassDecl *c = cls; c; c = c->parent)
        interfacesOf(c, interfaces);
      for (const Type *iface : interfaces) {
        const Field *member = iface->field(fn->name);
        if (member && takesParams(member->type, fn->sym)) {
          fn->sym->ret = member->type->ret();
          break;
        }
        if (const Overload *overload = iface->overload(fn->name))
          for (const Type *signature : overload->signatures)
            if (takesParams(signature, fn->sym))
              fn->sym->ret = signature->ret();
      }
    }
  }
}

void Sema::numberClasses() {
  // Each class before those down its chain, in the order declared.
  std::vector<ClassDecl *> order;
  std::function<void(ClassDecl *)> visit = [&](ClassDecl *cls) {
    order.push_back(cls);
    for (ClassDecl *child : cls->children)
      visit(child);
  };
  for (ClassDecl *cls : classes_)
    if (!cls->parent && !cls->literal)
      visit(cls);
  // The numbers: a class, then those down its chain; the classes of
  // literals after them all.
  unsigned next = 0;
  for (ClassDecl *cls : order)
    cls->id = ++next;
  for (ClassDecl *cls : llvm::reverse(order)) {
    cls->lastId = cls->id;
    for (const ClassDecl *child : cls->children)
      cls->lastId = std::max(cls->lastId, child->lastId);
  }
  for (ClassDecl *cls : classes_)
    if (cls->literal)
      cls->id = cls->lastId = ++next;
  classCount_ = next;
}

void Sema::buildVtable(ClassDecl *cls) {
  cls->vtable = cls->parent ? cls->parent->vtable : std::vector<FunctionDecl *>();
  for (FunctionDecl *fn : ownMethods(cls)) {
    if (fn->overrides) {
      fn->slot = fn->overrides->slot;
      if (fn->slot >= 0 && size_t(fn->slot) < cls->vtable.size())
        cls->vtable[size_t(fn->slot)] = fn->isAbstract ? nullptr : fn;
      continue;
    }
    if (fn->overridden || fn->isAbstract) {
      fn->slot = int(cls->vtable.size());
      cls->vtable.push_back(fn->isAbstract ? nullptr : fn);
    }
  }
}

void Sema::linkOverrides(ClassDecl *cls) {
  // What an instance of a generic class would report, the generic class
  // reports.
  bool muted = diags_.setMuted(false);
  diags_.setMuted(muted || cls->generic != nullptr);
  for (FunctionDecl *fn : ownMethods(cls)) {
    if (!fn->sym)
      continue;
    FunctionDecl *base = nullptr;
    std::vector<FunctionDecl *> inherited;
    if (cls->parent) {
      inherited = methodsNamed(cls->parent, fn->name, /*isStatic=*/false);
      for (FunctionDecl *v : inherited)
        if (v->sym && sameParams(v->sym, fn->sym))
          base = v;
    }
    const ClassDecl *owner = nullptr;
    const ClassField *field = cls->parent ? findField(cls->parent, fn->name, &owner) : nullptr;
    if (field && !field->method) {
      diags_.error(fn->loc(), "'" + fn->name + "' is a field of '" + owner->name +
                                  "', so a class that extends it cannot have a method of that "
                                  "name");
      continue;
    }
    if (!base) {
      if (fn->isOverride) {
        if (!cls->parent)
          diags_.error(fn->loc(), "'override' says that '" + fn->name +
                                      "' replaces a method of the class that '" + cls->name +
                                      "' extends, and it extends none");
        else if (inherited.empty())
          diags_.error(fn->loc(), "'" + fn->name + "' is marked 'override', but '" +
                                      cls->parent->name + "' has no method '" + fn->name + "'");
        else
          diags_.error(fn->loc(), "'" + fn->name + "' is marked 'override', but no method of '" +
                                      cls->parent->name + "' has its parameters: there " +
                                      (inherited.size() == 1 ? "is " : "are ") +
                                      variantList(inherited) +
                                      "; an override takes the same parameters");
      }
      // One more variant of the name: it must differ from the inherited ones.
      for (FunctionDecl *v : inherited)
        checkVariantPair(v, fn);
      continue;
    }
    // An arrow in a field is a field of each object in JavaScript, which a
    // method of a subclass would not replace.
    if (isArrowMethod(base) && !isArrowMethod(fn)) {
      diags_.error(fn->loc(), "'" + fn->name + "' is an arrow in a field of '" + base->cls->name +
                                  "', which each object holds itself in JavaScript, so a method of "
                                  "a class that extends it would never run; override it with a "
                                  "field too: '" +
                                  fn->name + " = (...) => ...'");
      continue;
    }
    if (base->access == Access::Private) {
      diags_.error(fn->loc(), "'" + fn->name + "' is private in class '" + base->cls->name +
                                  "', so a class that extends it cannot override it");
      continue;
    }
    if (fn->access > base->access)
      diags_.error(fn->loc(), "'" + fn->name + "' is " + accessName(base->access) + " in '" +
                                  base->cls->name + "', so its override cannot be " +
                                  accessName(fn->access));
    if (fn->isAbstract && !base->isAbstract)
      diags_.error(fn->loc(), "'" + fn->name + "' has a body in '" + base->cls->name +
                                  "', so it cannot be abstract again in '" + cls->name + "'");
    fn->overrides = base;
    for (FunctionDecl *p = base; p; p = p->overrides) {
      // After the classes are linked, only an instance comes; that its
      // method overrides one that no other class overrides means the
      // method tables would be laid out again.
      if (classesLinked_ && !p->overridden && p->slot < 0) {
        bool outer = diags_.setMuted(false);
        diags_.error(fn->loc(), "'" + fn->name + "' of '" + cls->type->name() + "' overrides '" +
                                    p->cls->type->name() + "." + p->name +
                                    "' only with these type arguments, which is not supported; give "
                                    "the method the same parameters in '" + p->cls->name + "'");
        diags_.setMuted(outer);
      }
      markOverridden(p);
    }
  }
  diags_.setMuted(muted);
}

const Type *Sema::inheritedReturnType(FuncSymbol *fn) {
  FunctionDecl *decl = fn->decl;
  if (!decl)
    return nullptr;
  if (auto found = returnSources_.find(decl); found != returnSources_.end())
    return found->second ? returnType(found->second->sym) : Type::voidTy();
  if (auto found = inheritsReturn_.find(decl); found != inheritsReturn_.end())
    return returnType(found->second->sym);
  return nullptr;
}

void Sema::noteInheritedReturn(const FunctionDecl *fn) {
  if (!fn || !fn->cls || fn->returnType || !fn->sym || !fn->sym->ret)
    return;
  std::string type = fn->sym->ret->name().str();
  if (const FunctionDecl *base = fn->overrides)
    diags_.note(base->loc(), "'" + fn->name + "' overrides '" + base->cls->name + "." + fn->name +
                                 "', so it returns '" + type +
                                 "' too; to return another type, "
                                 "declare it in '" +
                                 base->cls->name + "'");
}

//===----------------------------------------------------------------------===//
// What a class must implement
//===----------------------------------------------------------------------===//

void Sema::checkAbstract(ClassDecl *cls) {
  if (cls->literal)
    return;
  for (FunctionDecl *fn : ownMethods(cls))
    if (fn->isAbstract && !cls->isAbstract)
      diags_.error(fn->loc(), "abstract method '" + fn->name + "' needs an abstract class: '" +
                                  cls->name +
                                  "' can be created with 'new', and the method would "
                                  "have no body; write 'abstract class " +
                                  cls->name + "'");
  if (cls->isAbstract)
    return;
  std::vector<FunctionDecl *> missing;
  for (size_t slot = 0; slot < cls->vtable.size(); ++slot) {
    if (cls->vtable[slot])
      continue;
    // The abstract method of this slot, as the nearest class declares it.
    for (const ClassDecl *c = cls; c; c = c->parent) {
      FunctionDecl *found = nullptr;
      for (FunctionDecl *fn : ownMethods(c))
        if (fn->slot == int(slot))
          found = fn;
      if (found) {
        if (found->cls != cls)
          missing.push_back(found);
        break;
      }
    }
  }
  for (FunctionDecl *fn : missing)
    diags_.error(cls->loc(), "class '" + cls->name + "' does not implement the abstract method " +
                                 variantText(fn) + " of '" + fn->cls->name +
                                 "'; implement it, or make '" + cls->name + "' abstract too");
}

void Sema::checkImplements(ClassDecl *cls) {
  std::vector<const Type *> interfaces;
  interfacesOf(cls, interfaces);
  for (const Type *iface : interfaces) {
    // The interfaces an implemented one extends have their fields in it.
    if (!llvm::is_contained(cls->implemented, iface))
      continue;
    std::string prefix = "class '" + cls->name.str() + "' implements '" + iface->name().str() + "'";
    for (const Field &member : iface->fields()) {
      const ClassField *field = findField(cls, member.name);
      if (field && !field->method) {
        const Field *actual = cls->type->field(member.name);
        if (!actual || actual->type->isError() || member.type->isError())
          continue;
        // At the field, if the class declares it itself. The class may
        // narrow it to a type of the same references, or make it read-only:
        // code that writes through the interface is found by the check of
        // the whole program (sema_widen.cpp).
        llvm::SMLoc loc = cls->field(member.name) ? field->loc : cls->loc();
        if (actual->type != member.type && !sameReference(actual->type, member.type))
          diags_.error(loc, prefix + ", whose field '" + member.name + "' is '" +
                                member.type->name() + "', but in '" + cls->name + "' it is '" +
                                actual->type->name() +
                                "'; the types must be the same, or objects "
                                "of a type down the chain of the interface's, such as 'Dog' for "
                                "'Animal'");
        continue;
      }
      std::vector<FunctionDecl *> methods = methodsNamed(cls, member.name, /*isStatic=*/false);
      if (methods.empty()) {
        std::string type = member.type->isOptional() ? member.type->inner()->name().str()
                                                     : member.type->name().str();
        diags_.error(cls->loc(), prefix + ", but has no '" + member.name + "'; declare it, e.g. '" +
                                     member.name + (member.type->isOptional() ? "?" : "") + ": " +
                                     type + "'");
        continue;
      }
      if (!member.type->isFunction()) {
        diags_.error(methods[0]->loc(), "'" + member.name + "' is a field of type '" +
                                            member.type->name() + "' in '" + iface->name() +
                                            "', which '" + cls->name +
                                            "' implements, so it cannot be a method");
        continue;
      }
      if (!member.readonly && llvm::none_of(methods, isArrowMethod)) {
        diags_.error(methods[0]->loc(), "'" + member.name + "' is a field in '" + iface->name() +
                                            "' that code may replace, so in '" + cls->name +
                                            "', which implements it, it must be a field too: '" +
                                            member.name + " = (...) => ...'; or declare it in '" +
                                            iface->name() + "' as a method, '" + member.name +
                                            "(...): ...'");
        continue;
      }
      bool found = false;
      for (FunctionDecl *fn : methods) {
        if (!fn->sym || !takesParams(member.type, fn->sym))
          continue;
        found = true;
        const Type *ret = returnType(fn->sym);
        if (ret && !ret->isError() && ret != member.type->ret())
          diags_.error(fn->loc(), "'" + fn->name + "' returns '" + ret->name() + "', and '" +
                                      iface->name() + "', which '" + cls->name +
                                      "' implements, says it returns '" +
                                      member.type->ret()->name() + "'");
      }
      if (!found)
        diags_.error(methods[0]->loc(), prefix + ", whose '" + member.name + "' is '" +
                                            member.type->name() + "', but '" + cls->name + "." +
                                            member.name +
                                            "' takes other parameters: " + variantList(methods));
    }
    // Each signature of a method with several, by a variant of its own.
    for (const Overload &overload : iface->overloads()) {
      std::vector<FunctionDecl *> methods = methodsNamed(cls, overload.name, /*isStatic=*/false);
      for (const Type *signature : overload.signatures) {
        auto fn = llvm::find_if(methods, [&](const FunctionDecl *m) {
          return m->sym && takesParams(signature, m->sym);
        });
        if (fn == methods.end()) {
          diags_.error(cls->loc(), prefix + ", but has no variant of '" + overload.name +
                                       "' for its signature '" + signature->name() +
                                       "'; each signature needs a variant with these parameters");
          continue;
        }
        const Type *ret = returnType((*fn)->sym);
        if (ret && !ret->isError() && ret != signature->ret())
          diags_.error((*fn)->loc(), "'" + overload.name + "' returns '" + ret->name() +
                                         "', and '" + iface->name() + "', which '" + cls->name +
                                         "' implements, says it returns '" +
                                         signature->ret()->name() + "'");
      }
    }
  }
}

// A call `this.m()` in a constructor (or in the initializers of the fields)
// runs before the classes down the chain have set their fields: if one of
// them overrides m, its m would read fields that are not set yet (node gives
// undefined, or throws a TypeError). The same through the methods the
// constructor calls on `this`.
void Sema::checkConstructorCalls() {
  for (ClassDecl *cls : classes_) {
    // An instance of a generic class: its generic class reports.
    if (cls->literal || cls->children.empty() || cls->generic)
      continue;
    std::vector<FunctionDecl *> fns = cls->constructors;
    if (cls->init)
      fns.push_back(cls->init);
    for (FunctionDecl *ctor : fns) {
      llvm::SmallPtrSet<const FunctionDecl *, 8> seen;
      scanConstructorCalls(ctor, cls, nullptr, ctor, seen);
    }
  }
}

void Sema::scanConstructorCalls(const FunctionDecl *fn, const ClassDecl *cls,
                                const CallExpr *origin, const FunctionDecl *ctor,
                                llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) {
  if (!fn->body || !fn->thisVar || !seen.insert(fn).second)
    return;
  const VarSymbol *self = fn->thisVar;
  visitExprs(
      fn->body,
      [&](const Expr *e) {
        auto *call = dyn_cast<CallExpr>(e);
        // A call of the constructor is reported once, whatever it leads to.
        if (!call || !call->method || (origin && reportedCalls_.contains(origin)))
          return;
        auto *member = dyn_cast<MemberExpr>(call->callee);
        const Expr *object = member ? member->object : nullptr;
        while (auto *conv = llvm::dyn_cast_or_null<CastExpr>(object))
          object = conv->operand;
        auto *name = llvm::dyn_cast_or_null<NameExpr>(object);
        if (!name || name->var != self)
          return;
        FunctionDecl *target = call->method->decl;
        const CallExpr *site = origin ? origin : call;
        if (call->virtualCall || !cls->derivesFrom(target->cls)) {
          // The class down the chain whose method would run.
          const ClassDecl *by = target->cls;
          if (call->virtualCall) {
            std::function<const ClassDecl *(const ClassDecl *)> find =
                [&](const ClassDecl *c) -> const ClassDecl * {
              for (const ClassDecl *child : c->children) {
                for (FunctionDecl *other : ownMethods(child))
                  if (overridesTransitively(other, target))
                    return child;
                if (const ClassDecl *found = find(child))
                  return found;
              }
              return nullptr;
            };
            if (const ClassDecl *found = find(cls))
              by = found;
          }
          std::string what = ctor->isConstructor
                                 ? "the constructor of '" + cls->name.str() + "'"
                                 : "the initializers of the fields of '" + cls->name.str() + "'";
          diags_.error(site->loc(),
                       what + " calls '" + target->name.str() + "'" +
                           (site == call ? std::string() : " (through the methods it calls)") +
                           ", which '" + by->name +
                           "' overrides: it would run "
                           "before the fields of '" +
                           by->name + "' are set; call it once the object is made");
          reportedCalls_.insert(site);
          return;
        }
        // The methods each call of the constructor leads to.
        if (!origin)
          seen.clear();
        seen.insert(fn);
        scanConstructorCalls(target, cls, site, ctor, seen);
      },
      /*intoFunctions=*/false);
}

//===----------------------------------------------------------------------===//
// super
//===----------------------------------------------------------------------===//

// `super(args)`: the constructor of the class extended, chosen by the
// arguments among its variants (or those of the nearest class up the chain
// that has any). Then the initializers of this class's fields run, as in
// JavaScript, and `this` may be used.
Expr *Sema::checkSuperCall(CallExpr *call) {
  call->type = Type::voidTy();
  call->callee->type = Type::voidTy();
  call->superCall = true;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  FunctionDecl *ctor = currentDecl();
  if (!ctor || !ctor->isConstructor || !ctor->cls) {
    diags_.error(call->loc(), "'super(...)' can be called only in the constructor of a class that "
                              "extends another");
    checkArgsLoosely();
    return call;
  }
  ClassDecl *cls = ctor->cls;
  if (!cls->parent) {
    if (!cls->base) // with an error in `extends`, it was reported there
      diags_.error(call->loc(), "'super(...)' calls the constructor of the class that '" +
                                    cls->name + "' extends, and it extends none");
    checkArgsLoosely();
    return call;
  }
  if (!rootSupers_.contains(call)) {
    diags_.error(call->loc(), "'super(...)' must be a statement of its own in the body of the "
                              "constructor, not inside a block, a condition, a loop or another "
                              "expression: the fields are set right after it");
    checkArgsLoosely();
    return call;
  }
  if (!flow_.beforeSuper) {
    diags_.error(call->loc(), "'super(...)' is called a second time; the constructor of '" +
                                  cls->parent->name + "' runs once");
    checkArgsLoosely();
    return call;
  }
  std::vector<FunctionDecl *> ctors = constructorsOf(cls->parent);
  if (ctors.empty()) {
    if (!call->args.empty())
      diags_.error(call->loc(), "class '" + cls->parent->name +
                                    "' has no constructor, so "
                                    "'super()' takes no arguments");
    checkArgsLoosely();
  } else {
    checkAccess(ctors[0]->cls, ctors[0]->access, "constructor", call->loc());
    bool checked = false;
    if (FuncSymbol *sym = chooseVariant(ctors, call->args, call->spread, call->loc(), checked)) {
      resolveParams(sym);
      call->method = sym;
      std::vector<bool> given(checked ? call->args.size() : 0, true);
      checkArguments(call, sym->params, sym->minArgs, sym->hasRest, call->loc(), &given);
    }
  }
  // The fields up the chain are set now, and so are those with initializers.
  flow_.beforeSuper = false;
  for (const ClassField &f : cls->fields)
    if (!f.isStatic && ((f.init && !f.method) || f.redeclared) &&
        !llvm::is_contained(flow_.setFields, f.name))
      flow_.setFields.push_back(f.name);
  calledFunction(); // the constructors run
  return call;
}

// `super.m(args)`: the method m of the class extended, called directly on
// `this`, whatever the classes down the chain override.
Expr *Sema::checkSuperMethodCall(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  member->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  // An arrow sees the `super` of the method around it.
  FunctionDecl *method = nullptr;
  for (FunctionDecl *fn : llvm::reverse(functions_)) {
    if (fn->isArrow)
      continue;
    if (fn->takesThis() && !fn->cls->literal)
      method = fn;
    break;
  }
  if (!method || !method->cls->parent) {
    if (!method || !method->cls->base)
      diags_.error(member->object->loc(), "'super' is the class that the class of the method "
                                          "extends; it exists in the methods of a class that "
                                          "extends another");
    checkArgsLoosely();
    return call;
  }
  ClassDecl *parent = method->cls->parent;
  // The object is `this`, with its rules while the constructor sets fields.
  auto *self = ctx_.make<NameExpr>(member->object->loc(), "this");
  thisMember_ = member->name;
  member->object = checkThis(self);
  thisMember_ = {};
  if (member->object->type->isError()) {
    checkArgsLoosely();
    return call;
  }
  if (isInitThis(member->object) && !flow_.beforeSuper && !thisReady(flow_))
    diags_.error(member->loc(),
                 "'super." + member->name + "()' is called before every field is set: field '" +
                     firstUnset(flow_) + "' is not set yet, and the method may read it");
  std::vector<FunctionDecl *> methods = methodsNamed(parent, member->name, /*isStatic=*/false);
  if (methods.empty()) {
    if (findField(parent, member->name))
      diags_.error(member->loc(), "'" + member->name + "' is a field of '" + parent->name +
                                      "'; call the function it holds as 'this." + member->name +
                                      "(...)'");
    else
      diags_.error(member->loc(),
                   "class '" + parent->name + "' has no method '" + member->name + "'");
    checkArgsLoosely();
    return call;
  }
  checkAccess(methods[0]->cls, methods[0]->access, member->name, member->loc());
  // `super.on(...)` of an emitter, checked against its event map.
  if (isEmitterClass(parent))
    if (Expr *checked = checkEmitterCall(call, member, parent, /*super=*/true))
      return checked;
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym = chooseVariant(methods, call->args, call->spread, member->loc(), checked, &bound);
  if (!sym)
    return call;
  if (sym->decl->isAbstract) {
    diags_.error(member->loc(), "'" + member->name + "' is abstract in '" + sym->decl->cls->name +
                                    "': it has no body to call");
    checkArgsLoosely();
    return call;
  }
  resolveParams(sym);
  call->superCall = true;
  // A generic method of the class extended: the copy for these types.
  if (sym->isGeneric() && checked)
    return callGenericVariant(call, sym, bound, member->loc(), /*method=*/true);
  if (sym->isGeneric()) {
    checkGenericCall(call, sym, member->loc(), nullptr, member->typeArgs);
    call->method = call->func;
    call->func = nullptr;
    if (call->method)
      member->type = functionType(call->method);
    return call;
  }
  call->method = sym;
  member->type = functionType(sym);
  call->type = returnType(sym);
  std::vector<bool> given(checked ? call->args.size() : 0, true);
  return checkArguments(call, sym->params, sym->minArgs, sym->hasRest, member->loc(), &given);
}

//===----------------------------------------------------------------------===//
// Methods of interfaces with several signatures
//===----------------------------------------------------------------------===//

std::vector<FunctionDecl *> Sema::signatureVariants(const Type *iface, const Overload &overload) {
  std::vector<FunctionDecl *> &variants = signatureVariants_[{iface, overload.name}];
  if (!variants.empty())
    return variants;
  for (const Type *signature : overload.signatures) {
    auto *fn = ctx_.make<FunctionDecl>(llvm::SMLoc(), ctx_.keepName(overload.name));
    FuncSymbol *sym = ctx_.newFunc();
    sym->name = fn->name;
    sym->decl = fn;
    sym->params.assign(signature->params().begin(), signature->params().end());
    sym->minArgs = signature->minArgs();
    sym->hasRest = signature->hasRest();
    sym->ret = signature->ret();
    for (size_t i = 0; i < signature->params().size(); ++i) {
      Param param;
      param.name = ctx_.keepName(std::string(1, char('a' + i % 26)));
      param.rest = sym->hasRest && i + 1 == signature->params().size();
      param.optional = i >= sym->minArgs && !param.rest;
      param.declaredType = signature->params()[i];
      fn->params.push_back(param);
    }
    fn->sym = sym;
    variants.push_back(fn);
  }
  return variants;
}

Expr *Sema::checkOverloadCall(CallExpr *call, MemberExpr *member) {
  const Type *iface = member->object->type;
  const Overload &overload = *iface->overload(member->name);
  call->type = member->type = Type::error();
  std::vector<FunctionDecl *> variants = signatureVariants(iface, overload);
  bool checked = false;
  FuncSymbol *sym = chooseVariant(variants, call->args, call->spread, member->loc(), checked);
  if (!sym)
    return call;
  auto found = llvm::find(variants, sym->decl);
  call->signature = int(found - variants.begin());
  member->type = overload.signatures[size_t(call->signature)];
  call->type = sym->ret;
  calledFunction(); // a method runs
  std::vector<bool> given(checked ? call->args.size() : 0, true);
  return checkArguments(call, sym->params, sym->minArgs, sym->hasRest, member->loc(), &given);
}

Expr *Sema::checkOverloadMember(MemberExpr *member, const Type *expected) {
  const Type *iface = member->object->type;
  const Overload &overload = *iface->overload(member->name);
  member->type = Type::error();
  std::vector<FunctionDecl *> variants = signatureVariants(iface, overload);
  FuncSymbol *sym = variantForType(variants, expected, member);
  if (!sym)
    return member;
  member->signature = int(llvm::find(variants, sym->decl) - variants.begin());
  member->type = overload.signatures[size_t(member->signature)];
  return member;
}

} // namespace tz
