// Classes as values (docs/decorators.md, sections 5 and 6; docs/objects.md,
// section 12): `typeof C` and `Constructor<T>`, `new` of a class as a value,
// class expressions and the classes they make, mixins, and the class that a
// decorator gives in place of the one it decorates.
//
// `typeof C` is the class C itself, known when compiling, whatever holds it:
// a variable, a parameter of a copy of a generic function, the result of a
// mixin. `new` of it is `new C`, and its static members are C's.
// `Constructor<T>` is some class whose objects are T, known only when the
// program runs: any class whose objects are T converts to it, and `new` of
// it goes through the description of the class in the exe (see
// sema_reflect.cpp).
//
// A class expression makes a class when Sema checks it. In a copy of a
// generic function each copy makes its own: `Timestamped(User)` extends User
// and `Timestamped(Post)` extends Post, since the fields of an object of tz
// lie at places fixed when compiling. Such a function is a template: its body
// is checked only in its copies, each finding its own return type, the class
// it makes. A class made so is linked after the others, so a method of a name
// that some class expression of the program declares is called through the
// method table everywhere: the class may override it.
//
// A class decorator that returns a class that extends the class it got
// replaces it: from where the declaration runs on, the name means that class
// (`new Order(100).id`), as TypeScript reassigns the name.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// Whether a value of `type` may hold an object of `target`.
bool holdsType(const Type *type, const Type *target, llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (type == target)
    return true;
  if (!seen.insert(type).second)
    return false;
  switch (type->kind()) {
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return holdsType(type->inner(), target, seen);
  case Type::Kind::Union:
    return llvm::any_of(type->members(), [&](const Type *m) { return holdsType(m, target, seen); });
  case Type::Kind::Dict:
    return holdsType(type->keyType(), target, seen) || holdsType(type->valueType(), target, seen);
  case Type::Kind::Object:
    return llvm::any_of(type->fields(),
                        [&](const Field &f) { return holdsType(f.type, target, seen); });
  default:
    return false;
  }
}

} // namespace

const Type *Sema::constructedBy(const Type *type) {
  if (type->isConstructor())
    return type->constructed();
  const ClassDecl *cls = type->classRef();
  return cls ? cls->type : nullptr;
}

ClassDecl *Sema::finalClass(ClassDecl *cls) {
  while (cls && cls->replaced && cls->replacement)
    cls = cls->replacement;
  return cls;
}

const Type *Sema::classValueType(ClassDecl *cls) {
  cls = finalClass(cls);
  // A class without a name shows as what it extends.
  return Type::classRef(cls, cls->name.empty() && cls->type ? cls->type->name() : cls->name);
}

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

const Type *Sema::resolveConstructorType(const TypeRef &ref) {
  if (ref.args.size() != 1) {
    diags_.error(ref.loc, "'Constructor' takes the type of the objects the class makes, e.g. "
                          "'Constructor<User>', or nothing for any class");
    return Type::error();
  }
  const Type *made = resolveType(ref.args[0], /*allowVoid=*/false);
  if (made->isError())
    return made;
  // A class, an interface, `object`; in generic code a type parameter.
  if ((!made->isObject() || made->isTuple()) && !made->isParam()) {
    diags_.error(ref.args[0].loc, "a class makes objects, and '" + made->name() +
                                      "' is not an object type; give the type of its objects, "
                                      "e.g. 'Constructor<User>'");
    return Type::error();
  }
  return Type::constructorOf(made);
}

const Type *Sema::constructorArgs(llvm::SMLoc loc) {
  const ClassDecl *cls = declaringClass_;
  if (!cls || !cls->parent) {
    diags_.error(loc, "'ConstructorArgs' is the list of the parameters of the constructor of the "
                      "class extended, for the constructor of a class that extends another: "
                      "'constructor(...args: ConstructorArgs) { super(...args) }'");
    return Type::error();
  }
  std::vector<FunctionDecl *> ctors = constructorsOf(cls->parent);
  if (ctors.empty())
    return Type::tuple({});
  const std::string parent = cls->parent->type->name().str();
  if (ctors.size() > 1) {
    diags_.error(loc, "'ConstructorArgs' is the list of the parameters of the constructor of '" +
                          parent + "', which has several; write the parameters of the one to "
                                   "call");
    return Type::error();
  }
  FuncSymbol *sym = ctors[0]->sym;
  if (!sym) {
    diags_.error(loc, "'ConstructorArgs' needs the constructor of '" + parent +
                          "', which is declared after this class; move '" + parent + "' up");
    return Type::error();
  }
  resolveParams(sym);
  if (llvm::is_contained(sym->params, nullptr))
    return Type::error();
  std::vector<const Type *> elements(sym->params.begin(), sym->params.end());
  const Type *rest = nullptr;
  if (sym->hasRest) {
    rest = elements.back();
    elements.pop_back();
  }
  return Type::argumentList(elements, sym->minArgs, rest);
}

//===----------------------------------------------------------------------===//
// new
//===----------------------------------------------------------------------===//

Expr *Sema::checkValueNew(NewExpr *expr, const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
  };
  expr->callee = checkExpr(expr->callee);
  const Type *type = expr->callee->type;
  if (type->isError()) {
    checkArgsLoosely();
    return expr;
  }
  if (!type->isClassRef()) {
    diags_.error(expr->callee->loc(), "'new' makes an object of a class, and this is a '" +
                                          type->name() + "'");
    checkArgsLoosely();
    return expr;
  }
  // A class known when compiling: as `new C(...)`, after the class value.
  if (ClassDecl *cls = const_cast<ClassDecl *>(type->classRef())) {
    if (!cls->type) {
      diags_.error(expr->callee->loc(), "'new' of '" + cls->name +
                                            "' as a value is not supported; write 'new " +
                                            cls->name + "(...)' or a literal");
      checkArgsLoosely();
      return expr;
    }
    return checkClassNew(expr, cls, expected);
  }
  // A class known only when the program runs: its description makes the
  // object, checking the arguments against its constructor then.
  if (!expr->typeArgs.empty())
    diags_.error(expr->loc(), "a class known only when the program runs takes no type "
                              "arguments");
  expr->reflect = true;
  for (Expr *&arg : expr->args) {
    arg = checkExpr(arg);
    if (arg->type->isVoid())
      diags_.error(arg->loc(), "an expression of type 'void' has no value to pass");
    else
      arg = convert(arg, Type::unknown());
  }
  calledFunction(); // the code of the class runs
  reflectionUsed_ |= !onlyChecked();
  expr->type = type->constructed();
  return expr;
}

//===----------------------------------------------------------------------===//
// Class expressions
//===----------------------------------------------------------------------===//

Expr *Sema::checkClassExpr(ClassExpr *expr, const Type *expected) {
  (void)expected;
  ClassDecl *cls = expr->decl;
  // The same expression checked again (a body checked twice) keeps its
  // class.
  if (!cls->type)
    makeLateClass(cls);
  cls->usedAsValue = true;
  // Its static fields get their values here, the first time it runs, and
  // the decorators of its members run then.
  if (!cls->declared) {
    checkStaticFields(cls);
    if (hasDecorators(cls)) {
      ClassDecl *outer = std::exchange(currentClass_, cls);
      decorateClass(cls);
      currentClass_ = outer;
    }
  }
  expr->type = classValueType(cls);
  return expr;
}

void Sema::makeLateClass(ClassDecl *cls) {
  // In generic code that is no copy (a generic class, a trial copy of a
  // function), the class is only checked.
  cls->checkOnly = onlyChecked();
  // A class expression is declared here; a declaration with a value as its
  // base (`class A extends Mixin(B)`) was, with the types.
  if (!cls->type) {
    cls->id = ++classCount_;
    cls->type = Type::classType(cls->name.empty() ? "(anonymous class)" : cls->name, cls);
    classes_.push_back(cls);
  }
  if (module_ && !declModules_.count(cls))
    declModules_[cls] = module_;
  // The copy of a generic function around it, whose type parameters it
  // sees, and the variables of the functions around it, which it cannot.
  for (FunctionDecl *fn : llvm::reverse(functions_))
    if (fn->isInstance && !fn->typeArgs.empty()) {
      cls->outer = fn;
      break;
    }
  for (size_t i = 1; i < scopes_.size(); ++i)
    for (const auto &entry : scopes_[i])
      if (entry.second.var)
        cls->locals.push_back(entry.second.var->name);

  // The class it extends, which must be known when compiling.
  if (cls->baseExpr) {
    cls->baseExpr = checkExpr(cls->baseExpr);
    const Type *type = cls->baseExpr->type;
    ClassDecl *parent = const_cast<ClassDecl *>(type->classRef());
    llvm::SMLoc loc = cls->baseExpr->loc();
    if (type->isError()) {
      // Reported.
    } else if (type->isConstructor()) {
      diags_.error(loc, "the class extended must be known when compiling: the fields of an object "
                        "lie at places fixed then, and '" + type->name().str() +
                        "' is some class known only when the program runs (docs/decorators.md, "
                        "section 5); extend a class by its name, or take it in a generic "
                        "function, 'function Mixin<B extends Constructor>(Base: B)'");
    } else if (!type->isClassRef()) {
      diags_.error(loc, "a class can extend only a class, and this is a '" + type->name() + "'");
    } else if (!parent->type) {
      diags_.error(loc, "a class cannot extend '" + parent->name +
                            "' of JavaScript; extend a class of the program");
      parent = nullptr;
    } else if (parent->isGeneric()) {
      diags_.error(loc, "'" + parent->name + "' is generic; extend one of its instances, e.g. '" +
                            parent->name + "<int>'");
      parent = nullptr;
    } else if (isRuntimeClass(parent)) {
      diags_.error(loc, "a class cannot extend '" + parent->name +
                            "': only the functions of node:fs make its objects");
      parent = nullptr;
    }
    // A class only checked is no class down the chain of its parent, and
    // CodeGen lays out no type of it.
    if (parent) {
      cls->parent = parent;
      if (cls->checkOnly) {
        const_cast<Type *>(cls->type)->addBase(parent->type);
      } else {
        parent->children.push_back(cls);
        addBase(cls->type, parent->type);
      }
    }
    cls->base.reset();
  }
  if (cls->name.empty() && cls->parent)
    const_cast<Type *>(cls->type)->rename("(class extends " +
                                          cls->parent->type->name().str() + ")");

  ClassDecl *outerDeclaring = std::exchange(declaringClass_, cls);
  ClassDecl *outerClass = std::exchange(currentClass_, cls);
  resolveClass(cls);
  declareMethods(cls);
  currentClass_ = outerClass;
  declaringClass_ = outerDeclaring;
  // Its methods are functions of the file it is in.
  std::vector<FunctionDecl *> methods = cls->methods;
  methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
  if (cls->init)
    methods.push_back(cls->init);
  for (const ClassField &f : cls->fields)
    if (f.method)
      methods.push_back(f.method);
  if (Module *home = declModules_.lookup(cls))
    for (FunctionDecl *fn : methods)
      declModules_[fn] = home;
  // Linked as an instance of a generic class is, after the others: a class
  // made later still may override its methods.
  markLateOverrides(cls);
  if (classesLinked_) {
    linkOverrides(cls);
    buildVtable(cls);
  }
  if (checkingInstance_ && checkingInstance_->generic)
    lateOwners_[cls] = checkingInstance_;
  lateClasses_.push_back(cls);
}

void Sema::checkStaticFields(ClassDecl *cls) {
  ClassDecl *outer = std::exchange(currentClass_, cls);
  bool outerStatic = std::exchange(staticInit_, true);
  cls->declared = true;
  staticsSet_.clear();
  for (ClassField &f : cls->fields) {
    if (!f.isStatic || !f.var)
      continue;
    if (!f.init) {
      if (!f.var->type)
        f.var->type = Type::error();
      staticsSet_.insert(f.var);
      continue;
    }
    const Type *declared = f.var->type;
    if (declared && f.resolved && !f.type && !f.optional)
      declared = nullptr; // the type of a literal, which the value gives again
    f.init = checkExpr(f.init, declared && !declared->isError() ? declared : nullptr);
    staticsSet_.insert(f.var);
    if (declared) {
      f.init = coerce(f.init, declared, "static field '" + f.name.str() + "'");
      continue;
    }
    const Type *type = f.init->type;
    if (type->isVoid() || type->isUndefined()) {
      diags_.error(f.init->loc(), "cannot infer the type of static field '" + f.name +
                                      "' from this value; declare it, e.g. 'static " + f.name +
                                      ": int | undefined = undefined'");
      type = Type::error();
    }
    f.var->type = f.resolved = type;
  }
  staticInit_ = outerStatic;
  currentClass_ = outer;
}

void Sema::checkLateClasses() {
  for (; lateChecked_ < lateClasses_.size(); ++lateChecked_) {
    ClassDecl *cls = lateClasses_[lateChecked_];
    if (cls->checkOnly)
      continue;
    if (Module *home = declModules_.lookup(cls); home && scopes_.size() == 1)
      useModule(home);
    // The code of a class a copy of a template made: its errors are the
    // copy's.
    Instance *owner = lateOwners_.lookup(cls);
    Instance *outerInstance = checkingInstance_;
    std::vector<Diagnostics::Captured> *outerCapture = diags_.capture();
    if (owner) {
      checkingInstance_ = owner;
      diags_.setCapture(&lateErrors_[owner]);
    }
    flow_ = Flow();
    checkClassBodies(cls);
    checkingInstance_ = outerInstance;
    diags_.setCapture(outerCapture);
  }
  // Reported at the calls that made the copies, once each.
  for (auto &[instance, errors] : lateErrors_) {
    if (errors.empty() || instance->failed)
      continue;
    instance->failed = true;
    instance->message = errors.front().message;
    instance->errorLoc = errors.front().loc;
    reportInstance(instance, instance->callLoc, instance->concrete);
  }
}

void Sema::markLateOverrides(SourceFile &program) {
  // The methods that classes made later declare, by name.
  auto note = [&](const ClassDecl *cls) {
    for (const FunctionDecl *fn : cls->methods)
      if (!fn->isStatic)
        lateMethodNames_.insert(fn->name);
    for (const ClassField &f : cls->fields)
      if (!f.isStatic && f.init && isa<FunctionExpr>(f.init))
        lateMethodNames_.insert(f.name);
  };
  for (Stmt *item : program.items) {
    if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->baseExpr)
      note(cls);
    visitExprs(item, [&](const Expr *e) {
      if (auto *ce = dyn_cast<ClassExpr>(e); ce && (ce->decl->baseExpr || ce->decl->base))
        note(ce->decl);
    });
  }
  for (ClassDecl *cls : classes_)
    markLateOverrides(cls);
}

void Sema::markLateOverrides(ClassDecl *cls) {
  if (lateMethodNames_.empty() || cls->literal)
    return;
  for (FunctionDecl *fn : ownMethods(cls))
    if (lateMethodNames_.contains(fn->name)) {
      fn->overridden = true;
      fn->openToLate = true;
    }
}

void Sema::bindOuterTypeParams(const ClassDecl *cls) {
  if (const FunctionDecl *outer = cls ? cls->outer : nullptr)
    bindTypeParams(outer->typeParams, outer->typeArgs);
}

//===----------------------------------------------------------------------===//
// Templates
//===----------------------------------------------------------------------===//

bool Sema::hasClassExpr(const FunctionDecl *fn) {
  if (!fn || !fn->body)
    return false;
  auto found = templates_.find(fn);
  if (found != templates_.end())
    return found->second;
  bool has = false;
  visitExprs(fn->body, [&](const Expr *e) { has |= isa<ClassExpr>(e); });
  templates_[fn] = has;
  return has;
}

bool Sema::isTemplate(const FuncSymbol *generic) {
  return generic && generic->isGeneric() && generic->decl && !generic->decl->isInstance &&
         hasClassExpr(generic->decl);
}

void Sema::checkTemplateInstance(Instance *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  FunctionDecl *decl = instance->decl;
  BodyState saved = enterBody(decl);
  checkingInstance_ = instance;
  diags_.setCapture(&errors);
  // The call runs it now: in top-level code, the globals declared later are
  // not there yet.
  if (!topLevelDone_)
    early_ = decl->sym;
  flow_ = Flow();
  if (decl->sym->ret)
    checkFunctionBody(decl);
  else
    checkAndInfer(decl);
  leaveBody(saved);
  instance->checking = false;
  instance->checked = true;
  if (errors.empty())
    return;
  instance->failed = true;
  instance->message = errors.front().message;
  instance->errorLoc = errors.front().loc;
}

//===----------------------------------------------------------------------===//
// The class a decorator gives
//===----------------------------------------------------------------------===//

void Sema::replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc) {
  Type *mine = const_cast<Type *>(cls->type), *theirs = const_cast<Type *>(replacement->type);
  // An object a static field holds was made as declared, before the
  // decorator ran: it would not be of the class the name means from here on.
  for (const ClassField &f : cls->fields) {
    llvm::SmallPtrSet<const Type *, 8> seen;
    if (f.isStatic && f.var && f.var->type && holdsType(f.var->type, mine, seen)) {
      diags_.error(f.loc, "the decorator gives '" + cls->name + "' another class in its place, "
                          "and static field '" + f.name + "' holds an object made before, of the "
                          "class as declared; make that object where it is used, or keep it "
                          "outside the class");
      diags_.note(loc, "the decorator is here");
      return;
    }
  }
  // The name means the replacement from here on, and so does the type every
  // annotation resolved so far gives it: the types change places (see
  // Type::swapClasses). The class as declared keeps another one, which
  // `this` has in its methods.
  std::string name = mine->name().str();
  Type::swapClasses(mine, theirs);
  cls->type = theirs;
  replacement->type = mine;
  theirs->rename(name + " (as declared)");
  if (!llvm::is_contained(file_->subtypes, mine))
    file_->subtypes.push_back(mine);
  std::vector<ClassDecl *> declared;
  for (ClassDecl *child : cls->children) {
    if (child == replacement)
      continue;
    // A class between them, as `Tagged(Timestamped(cls))` makes: it extends
    // the class as declared.
    if (replacement->derivesFrom(child)) {
      const_cast<Type *>(child->type)->replaceBase(mine, theirs);
      continue;
    }
    declared.push_back(child);
  }
  // A class declared to extend it extends the replacement, as in
  // JavaScript: its fields, after those of the replacement, and its method
  // table are found again.
  std::function<void(ClassDecl *)> again = [&](ClassDecl *c) {
    const_cast<Type *>(c->type)->forgetFields();
    bool muted = diags_.setMuted(true);
    linkOverrides(c);
    diags_.setMuted(muted);
    buildVtable(c);
    for (ClassDecl *child : c->children)
      again(child);
  };
  for (ClassDecl *child : declared) {
    for (const ClassField &f : child->fields)
      if (!f.isStatic && replacement->field(f.name)) {
        diags_.error(f.loc, "'" + child->name + "' extends '" + cls->name + "', whose decorator "
                                "gives a class with a field '" + f.name + "' of its own in its "
                                "place; give this field another name");
        return;
      }
    llvm::erase(cls->children, child);
    replacement->children.push_back(child);
    child->parent = replacement;
    again(child);
  }
  cls->replacement = replacement;
  cls->replaced = true;
  replacement->usedAsValue = true;
}

} // namespace tz
