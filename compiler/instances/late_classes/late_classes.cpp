#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

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

}

LateClasses::LateClasses(Checker &checker) : checker_(checker) {}

Expr *LateClasses::checkClassExpr(ClassExpr *expr, const Type *expected) {
  (void)expected;
  ClassDecl *cls = expr->decl;
  if (!cls->type) {
    makeLateClass(cls);
  }
  cls->usedAsValue = true;
  if (!cls->declared) {
    checkStaticFields(cls);
    if (ClassChecker::hasDecorators(cls)) {
      ClassDecl *outer = std::exchange(checker_.currentClass_, cls);
      checker_.decoratorChecker().decorateClass(cls);
      checker_.currentClass_ = outer;
    }
  }
  expr->type = checker_.classValueChecker().classValueType(cls);

  return expr;
}

void LateClasses::makeLateClass(ClassDecl *cls) {
  cls->checkOnly = checker_.reflectChecker().onlyChecked();
  if (!cls->type) {
    cls->id = ++checker_.classCount_;
    cls->type = Type::classType(cls->name.empty() ? "(anonymous class)" : cls->name, cls);
    checker_.classes_.push_back(cls);
  }
  if (checker_.module_ && !checker_.declModules_.count(cls)) {
    checker_.declModules_[cls] = checker_.module_;
  }
  for (FunctionDecl *fn : llvm::reverse(checker_.functions_)) {
    if (fn->isInstance && !fn->typeArgs.empty()) {
      cls->outer = fn;
      break;
    }
  }
  for (size_t i = 1; i < checker_.scopes_.size(); ++i) {
    for (const auto &entry : checker_.scopes_[i]) {
      if (entry.second.var) {
        cls->locals.push_back(entry.second.var->name);
      }
    }
  }

  if (cls->baseExpr) {
    cls->baseExpr = checker_.exprChecker().checkExpr(cls->baseExpr);
    const Type *type = cls->baseExpr->type;
    ClassDecl *parent = const_cast<ClassDecl *>(type->classRef());
    llvm::SMLoc loc = cls->baseExpr->loc();
    if (type->isError()) {
    } else if (type->isConstructor()) {
      checker_.diags_.error(
          loc, "the class extended must be known when compiling: the fields of an object "
               "lie at places fixed then, and '" +
                   type->name().str() +
                   "' is some class known only when the program runs (docs/decorators.md, "
                   "section 5); extend a class by its name, or take it in a generic "
                   "function, 'function Mixin<B extends Constructor>(Base: B)'");
    } else if (!type->isClassRef()) {
      checker_.diags_.error(loc, "a class can extend only a class, and this is a '" + type->name() +
                                     "'");
    } else if (!parent->type) {
      checker_.diags_.error(loc, "a class cannot extend '" + parent->name +
                                     "' of JavaScript; extend a class of the program");
      parent = nullptr;
    } else if (parent->isGeneric()) {
      checker_.diags_.error(loc, "'" + parent->name +
                                     "' is generic; extend one of its instances, e.g. '" +
                                     parent->name + "<int>'");
      parent = nullptr;
    } else if (checker_.fsDeclarer().isRuntimeClass(parent)) {
      checker_.diags_.error(loc, "a class cannot extend '" + parent->name +
                                     "': only the functions of node:fs make its objects");
      parent = nullptr;
    }
    if (parent) {
      cls->parent = parent;
      if (cls->checkOnly) {
        const_cast<Type *>(cls->type)->addBase(parent->type);
      } else {
        parent->children.push_back(cls);
        checker_.classBases().addBase(cls->type, parent->type);
      }
    }
    cls->base.reset();
  }
  if (cls->name.empty() && cls->parent) {
    const_cast<Type *>(cls->type)->rename("(class extends " + cls->parent->type->name().str() +
                                          ")");
  }

  ClassDecl *outerDeclaring = std::exchange(checker_.declaringClass_, cls);
  ClassDecl *outerClass = std::exchange(checker_.currentClass_, cls);
  checker_.classDeclarer().resolveClass(cls);
  checker_.classDeclarer().declareMethods(cls);
  checker_.currentClass_ = outerClass;
  checker_.declaringClass_ = outerDeclaring;
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
  if (Module *home = checker_.declModules_.lookup(cls)) {
    for (FunctionDecl *fn : methods) {
      checker_.declModules_[fn] = home;
    }
  }
  markLateOverrides(cls);
  if (checker_.classesLinked_) {
    checker_.classLinks().linkOverrides(cls);
    checker_.classLinks().buildVtable(cls);
  }
  if (checker_.checkingInstance_ && checker_.checkingInstance_->generic) {
    checker_.lateOwners_[cls] = checker_.checkingInstance_;
  }
  checker_.lateClasses_.push_back(cls);
}

void LateClasses::checkStaticFields(ClassDecl *cls) {
  ClassDecl *outer = std::exchange(checker_.currentClass_, cls);
  bool outerStatic = std::exchange(checker_.staticInit_, true);
  cls->declared = true;
  checker_.staticsSet_.clear();
  for (ClassField &f : cls->fields) {
    if (!f.isStatic || !f.var) {
      continue;
    }
    if (!f.init) {
      if (!f.var->type) {
        f.var->type = Type::error();
      }
      checker_.staticsSet_.insert(f.var);
      continue;
    }
    const Type *declared = f.var->type;
    if (declared && f.resolved && !f.type && !f.optional) {
      declared = nullptr;
    }
    f.init = checker_.exprChecker().checkExpr(f.init, declared && !declared->isError() ? declared
                                                                                       : nullptr);
    checker_.staticsSet_.insert(f.var);
    if (declared) {
      f.init =
          checker_.typeRelations().coerce(f.init, declared, "static field '" + f.name.str() + "'");
      continue;
    }
    const Type *type = f.init->type;
    if (type->isVoid() || type->isUndefined()) {
      checker_.diags_.error(f.init->loc(), "cannot infer the type of static field '" + f.name +
                                               "' from this value; declare it, e.g. 'static " +
                                               f.name + ": int | undefined = undefined'");
      type = Type::error();
    }
    f.var->type = f.resolved = type;
  }
  checker_.staticInit_ = outerStatic;
  checker_.currentClass_ = outer;
}

void LateClasses::checkLateClasses() {
  for (; checker_.lateChecked_ < checker_.lateClasses_.size(); ++checker_.lateChecked_) {
    ClassDecl *cls = checker_.lateClasses_[checker_.lateChecked_];
    if (cls->checkOnly) {
      continue;
    }
    if (Module *home = checker_.declModules_.lookup(cls); home && checker_.scopes_.size() == 1) {
      checker_.moduleDeclarer().useModule(home);
    }
    Checker::Instance *owner = checker_.lateOwners_.lookup(cls);
    Checker::Instance *outerInstance = checker_.checkingInstance_;
    std::vector<Diagnostics::Captured> *outerCapture = checker_.diags_.capture();
    if (owner) {
      checker_.checkingInstance_ = owner;
      checker_.diags_.setCapture(&checker_.lateErrors_[owner]);
    }
    checker_.flow_ = Checker::Flow();
    checker_.classChecker().checkClassBodies(cls);
    checker_.checkingInstance_ = outerInstance;
    checker_.diags_.setCapture(outerCapture);
  }
  for (auto &[instance, errors] : checker_.lateErrors_) {
    if (errors.empty() || instance->failed) {
      continue;
    }
    instance->failed = true;
    instance->message = errors.front().message;
    instance->errorLoc = errors.front().loc;
    checker_.genericInstantiator().reportInstance(instance, instance->callLoc, instance->concrete);
  }
}

void LateClasses::markLateOverrides(SourceFile &program) {
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
  for (ClassDecl *cls : checker_.classes_) {
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
    checker_.typeParams().bindTypeParams(outer->typeParams, outer->typeArgs);
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

void LateClasses::checkTemplateInstance(Checker::Instance *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  FunctionDecl *decl = instance->decl;
  Checker::BodyState saved = checker_.returnInferrer().enterBody(decl);
  checker_.checkingInstance_ = instance;
  checker_.diags_.setCapture(&errors);
  if (!checker_.topLevelDone_) {
    checker_.early_ = decl->sym;
  }
  checker_.flow_ = Checker::Flow();
  if (decl->sym->ret) {
    checker_.functionChecker().checkFunctionBody(decl);
  } else {
    checker_.returnInferrer().checkAndInfer(decl);
  }
  checker_.returnInferrer().leaveBody(saved);
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
  for (const ClassField &f : cls->fields) {
    llvm::SmallPtrSet<const Type *, 8> seen;
    if (f.isStatic && f.var && f.var->type && holdsType(f.var->type, mine, seen)) {
      checker_.diags_.error(f.loc,
                            "the decorator gives '" + cls->name +
                                "' another class in its place, "
                                "and static field '" +
                                f.name +
                                "' holds an object made before, of the "
                                "class as declared; make that object where it is used, or keep it "
                                "outside the class");
      checker_.diags_.note(loc, "the decorator is here");
      return;
    }
  }
  std::string name = mine->name().str();
  Type::swapClasses(mine, theirs);
  cls->type = theirs;
  replacement->type = mine;
  theirs->rename(name + " (as declared)");
  if (!llvm::is_contained(checker_.file_->subtypes, mine)) {
    checker_.file_->subtypes.push_back(mine);
  }
  std::vector<ClassDecl *> declared;
  for (ClassDecl *child : cls->children) {
    if (child == replacement) {
      continue;
    }
    if (replacement->derivesFrom(child)) {
      const_cast<Type *>(child->type)->replaceBase(mine, theirs);
      continue;
    }
    declared.push_back(child);
  }
  std::function<void(ClassDecl *)> again = [&](ClassDecl *c) {
    const_cast<Type *>(c->type)->forgetFields();
    bool muted = checker_.diags_.setMuted(true);
    checker_.classLinks().linkOverrides(c);
    checker_.diags_.setMuted(muted);
    checker_.classLinks().buildVtable(c);
    for (ClassDecl *child : c->children) {
      again(child);
    }
  };
  for (ClassDecl *child : declared) {
    for (const ClassField &f : child->fields) {
      if (!f.isStatic && replacement->field(f.name)) {
        checker_.diags_.error(f.loc, "'" + child->name + "' extends '" + cls->name +
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

}
