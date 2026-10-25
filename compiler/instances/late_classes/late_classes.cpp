#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// Whether a value of `type` may hold an object of `target`.
bool holdsType(const Type *type, const Type *target, llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (type == target) {
    return true;
  }
  if (!seen.insert(type).second) {
    return false;
  }
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

LateClasses::LateClasses(Sema &sema) : sema_(sema) {}

Expr *LateClasses::checkClassExpr(ClassExpr *expr, const Type *expected) {
  (void)expected;
  ClassDecl *cls = expr->decl;
  // The same expression checked again (a body checked twice) keeps its
  // class.
  if (!cls->type) {
    makeLateClass(cls);
  }
  cls->usedAsValue = true;
  // Its static fields get their values here, the first time it runs, and
  // the decorators of its members run then.
  if (!cls->declared) {
    checkStaticFields(cls);
    if (ClassChecker::hasDecorators(cls)) {
      ClassDecl *outer = std::exchange(sema_.currentClass_, cls);
      sema_.decoratorChecker().decorateClass(cls);
      sema_.currentClass_ = outer;
    }
  }
  expr->type = sema_.classValueChecker().classValueType(cls);

  return expr;
}

void LateClasses::makeLateClass(ClassDecl *cls) {
  // In generic code that is no copy (a generic class, a trial copy of a
  // function), the class is only checked.
  cls->checkOnly = sema_.reflectChecker().onlyChecked();
  // A class expression is declared here; a declaration with a value as its
  // base (`class A extends Mixin(B)`) was, with the types.
  if (!cls->type) {
    cls->id = ++sema_.classCount_;
    cls->type = Type::classType(cls->name.empty() ? "(anonymous class)" : cls->name, cls);
    sema_.classes_.push_back(cls);
  }
  if (sema_.module_ && !sema_.declModules_.count(cls)) {
    sema_.declModules_[cls] = sema_.module_;
  }
  // The copy of a generic function around it, whose type parameters it
  // sees, and the variables of the functions around it, which it cannot.
  for (FunctionDecl *fn : llvm::reverse(sema_.functions_)) {
    if (fn->isInstance && !fn->typeArgs.empty()) {
      cls->outer = fn;
      break;
    }
  }
  for (size_t i = 1; i < sema_.scopes_.size(); ++i) {
    for (const auto &entry : sema_.scopes_[i]) {
      if (entry.second.var) {
        cls->locals.push_back(entry.second.var->name);
      }
    }
  }

  // The class it extends, which must be known when compiling.
  if (cls->baseExpr) {
    cls->baseExpr = sema_.exprChecker().checkExpr(cls->baseExpr);
    const Type *type = cls->baseExpr->type;
    ClassDecl *parent = const_cast<ClassDecl *>(type->classRef());
    llvm::SMLoc loc = cls->baseExpr->loc();
    if (type->isError()) {
      // Reported.
    } else if (type->isConstructor()) {
      sema_.diags_.error(loc, "the class extended must be known when compiling: the fields of an object "
                        "lie at places fixed then, and '" +
                            type->name().str() +
                            "' is some class known only when the program runs (docs/decorators.md, "
                            "section 5); extend a class by its name, or take it in a generic "
                            "function, 'function Mixin<B extends Constructor>(Base: B)'");
    } else if (!type->isClassRef()) {
      sema_.diags_.error(loc, "a class can extend only a class, and this is a '" + type->name() + "'");
    } else if (!parent->type) {
      sema_.diags_.error(loc, "a class cannot extend '" + parent->name +
                            "' of JavaScript; extend a class of the program");
      parent = nullptr;
    } else if (parent->isGeneric()) {
      sema_.diags_.error(loc, "'" + parent->name + "' is generic; extend one of its instances, e.g. '" +
                            parent->name + "<int>'");
      parent = nullptr;
    } else if (sema_.fsDeclarer().isRuntimeClass(parent)) {
      sema_.diags_.error(loc, "a class cannot extend '" + parent->name +
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
        sema_.classBases().addBase(cls->type, parent->type);
      }
    }
    cls->base.reset();
  }
  if (cls->name.empty() && cls->parent) {
    const_cast<Type *>(cls->type)->rename("(class extends " + cls->parent->type->name().str() +
                                          ")");
  }

  ClassDecl *outerDeclaring = std::exchange(sema_.declaringClass_, cls);
  ClassDecl *outerClass = std::exchange(sema_.currentClass_, cls);
  sema_.classDeclarer().resolveClass(cls);
  sema_.classDeclarer().declareMethods(cls);
  sema_.currentClass_ = outerClass;
  sema_.declaringClass_ = outerDeclaring;
  // Its methods are functions of the file it is in.
  std::vector<FunctionDecl *> methods = cls->methods;
  methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
  if (cls->init) {
    methods.push_back(cls->init);
  }
  for (const ClassField &f : cls->fields) {
    if (f.method) {
      methods.push_back(f.method);
    }
  }
  if (Module *home = sema_.declModules_.lookup(cls)) {
    for (FunctionDecl *fn : methods) {
      sema_.declModules_[fn] = home;
    }
  }
  // Linked as an instance of a generic class is, after the others: a class
  // made later still may override its methods.
  markLateOverrides(cls);
  if (sema_.classesLinked_) {
    sema_.classLinks().linkOverrides(cls);
    sema_.classLinks().buildVtable(cls);
  }
  if (sema_.checkingInstance_ && sema_.checkingInstance_->generic) {
    sema_.lateOwners_[cls] = sema_.checkingInstance_;
  }
  sema_.lateClasses_.push_back(cls);
}

void LateClasses::checkStaticFields(ClassDecl *cls) {
  ClassDecl *outer = std::exchange(sema_.currentClass_, cls);
  bool outerStatic = std::exchange(sema_.staticInit_, true);
  cls->declared = true;
  sema_.staticsSet_.clear();
  for (ClassField &f : cls->fields) {
    if (!f.isStatic || !f.var) {
      continue;
    }
    if (!f.init) {
      if (!f.var->type) {
        f.var->type = Type::error();
      }
      sema_.staticsSet_.insert(f.var);
      continue;
    }
    const Type *declared = f.var->type;
    if (declared && f.resolved && !f.type && !f.optional) {
      declared = nullptr; // the type of a literal, which the value gives again
    }
    f.init = sema_.exprChecker().checkExpr(f.init, declared && !declared->isError() ? declared : nullptr);
    sema_.staticsSet_.insert(f.var);
    if (declared) {
      f.init = sema_.typeRelations().coerce(f.init, declared, "static field '" + f.name.str() + "'");
      continue;
    }
    const Type *type = f.init->type;
    if (type->isVoid() || type->isUndefined()) {
      sema_.diags_.error(f.init->loc(), "cannot infer the type of static field '" + f.name +
                                      "' from this value; declare it, e.g. 'static " + f.name +
                                      ": int | undefined = undefined'");
      type = Type::error();
    }
    f.var->type = f.resolved = type;
  }
  sema_.staticInit_ = outerStatic;
  sema_.currentClass_ = outer;
}

void LateClasses::checkLateClasses() {
  for (; sema_.lateChecked_ < sema_.lateClasses_.size(); ++sema_.lateChecked_) {
    ClassDecl *cls = sema_.lateClasses_[sema_.lateChecked_];
    if (cls->checkOnly) {
      continue;
    }
    if (Module *home = sema_.declModules_.lookup(cls); home && sema_.scopes_.size() == 1) {
      sema_.moduleDeclarer().useModule(home);
    }
    // The code of a class a copy of a template made: its errors are the
    // copy's.
    Sema::Instance *owner = sema_.lateOwners_.lookup(cls);
    Sema::Instance *outerInstance = sema_.checkingInstance_;
    std::vector<Diagnostics::Captured> *outerCapture = sema_.diags_.capture();
    if (owner) {
      sema_.checkingInstance_ = owner;
      sema_.diags_.setCapture(&sema_.lateErrors_[owner]);
    }
    sema_.flow_ = Sema::Flow();
    sema_.classChecker().checkClassBodies(cls);
    sema_.checkingInstance_ = outerInstance;
    sema_.diags_.setCapture(outerCapture);
  }
  // Reported at the calls that made the copies, once each.
  for (auto &[instance, errors] : sema_.lateErrors_) {
    if (errors.empty() || instance->failed) {
      continue;
    }
    instance->failed = true;
    instance->message = errors.front().message;
    instance->errorLoc = errors.front().loc;
    sema_.genericInstantiator().reportInstance(instance, instance->callLoc, instance->concrete);
  }
}

void LateClasses::markLateOverrides(SourceFile &program) {
  // The methods that classes made later declare, by name.
  auto note = [&](const ClassDecl *cls) {
    for (const FunctionDecl *fn : cls->methods) {
      if (!fn->isStatic) {
        lateMethodNames_.insert(fn->name);
      }
    }
    for (const ClassField &f : cls->fields) {
      if (!f.isStatic && f.init && isa<FunctionExpr>(f.init)) {
        lateMethodNames_.insert(f.name);
      }
    }
  };
  for (Stmt *item : program.items) {
    if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->baseExpr) {
      note(cls);
    }
    visitExprs(item, [&](const Expr *e) {
      if (auto *ce = dyn_cast<ClassExpr>(e); ce && (ce->decl->baseExpr || ce->decl->base)) {
        note(ce->decl);
      }
    });
  }
  for (ClassDecl *cls : sema_.classes_) {
    markLateOverrides(cls);
  }
}

void LateClasses::markLateOverrides(ClassDecl *cls) {
  if (lateMethodNames_.empty() || cls->literal) {
    return;
  }
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    if (lateMethodNames_.contains(fn->name)) {
      fn->overridden = true;
      fn->openToLate = true;
    }
  }
}

void LateClasses::bindOuterTypeParams(const ClassDecl *cls) {
  if (const FunctionDecl *outer = cls ? cls->outer : nullptr) {
    sema_.typeParams().bindTypeParams(outer->typeParams, outer->typeArgs);
  }
}

bool LateClasses::hasClassExpr(const FunctionDecl *fn) {
  if (!fn || !fn->body) {
    return false;
  }
  auto found = templates_.find(fn);
  if (found != templates_.end()) {
    return found->second;
  }
  bool has = false;
  visitExprs(fn->body, [&](const Expr *e) { has |= isa<ClassExpr>(e); });
  templates_[fn] = has;

  return has;
}

bool LateClasses::isTemplate(const FuncSymbol *generic) {
  return generic && generic->isGeneric() && generic->decl && !generic->decl->isInstance &&
         hasClassExpr(generic->decl);
}

void LateClasses::checkTemplateInstance(Sema::Instance *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  FunctionDecl *decl = instance->decl;
  Sema::BodyState saved = sema_.returnInferrer().enterBody(decl);
  sema_.checkingInstance_ = instance;
  sema_.diags_.setCapture(&errors);
  // The call runs it now: in top-level code, the globals declared later are
  // not there yet.
  if (!sema_.topLevelDone_) {
    sema_.early_ = decl->sym;
  }
  sema_.flow_ = Sema::Flow();
  if (decl->sym->ret) {
    sema_.functionChecker().checkFunctionBody(decl);
  } else {
    sema_.returnInferrer().checkAndInfer(decl);
  }
  sema_.returnInferrer().leaveBody(saved);
  instance->checking = false;
  instance->checked = true;
  if (errors.empty()) {
    return;
  }
  instance->failed = true;
  instance->message = errors.front().message;
  instance->errorLoc = errors.front().loc;
}

void LateClasses::replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc) {
  Type *mine = const_cast<Type *>(cls->type), *theirs = const_cast<Type *>(replacement->type);
  // An object a static field holds was made as declared, before the
  // decorator ran: it would not be of the class the name means from here on.
  for (const ClassField &f : cls->fields) {
    llvm::SmallPtrSet<const Type *, 8> seen;
    if (f.isStatic && f.var && f.var->type && holdsType(f.var->type, mine, seen)) {
      sema_.diags_.error(f.loc, "the decorator gives '" + cls->name +
                              "' another class in its place, "
                              "and static field '" +
                              f.name +
                              "' holds an object made before, of the "
                              "class as declared; make that object where it is used, or keep it "
                              "outside the class");
      sema_.diags_.note(loc, "the decorator is here");
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
  if (!llvm::is_contained(sema_.file_->subtypes, mine)) {
    sema_.file_->subtypes.push_back(mine);
  }
  std::vector<ClassDecl *> declared;
  for (ClassDecl *child : cls->children) {
    if (child == replacement) {
      continue;
    }
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
    bool muted = sema_.diags_.setMuted(true);
    sema_.classLinks().linkOverrides(c);
    sema_.diags_.setMuted(muted);
    sema_.classLinks().buildVtable(c);
    for (ClassDecl *child : c->children) {
      again(child);
    }
  };
  for (ClassDecl *child : declared) {
    for (const ClassField &f : child->fields) {
      if (!f.isStatic && replacement->field(f.name)) {
        sema_.diags_.error(f.loc, "'" + child->name + "' extends '" + cls->name +
                                "', whose decorator "
                                "gives a class with a field '" +
                                f.name +
                                "' of its own in its "
                                "place; give this field another name");
        return;
      }
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
