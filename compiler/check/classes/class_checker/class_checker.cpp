#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/inherit_checker/inherit_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

void collectMemberCalls(const Stmt *body, llvm::StringSet<> &names) {
  visitExprs(body, [&](const Expr *e) {
    if (auto *call = dyn_cast<CallExpr>(e)) {
      if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
        names.insert(member->name);
      }
    }
  });
}

ClassChecker::ClassChecker(Sema &sema) : sema_(sema) {}

void ClassChecker::checkClassStmt(ClassDecl *cls) {
  if (sema_.scopes_.size() != 1 || !sema_.functions_.empty() || !cls->type) {
    sema_.diags_.error(cls->loc(), "a class can be declared only at the top level of a file; here, "
                             "make it with a class expression, 'const " +
                                 cls->name + " = class { ... }'");
    return;
  }
  // A class whose base is a value (`extends Mixin(Base)`) is made here,
  // where that value is.
  if (cls->baseExpr && !cls->resolved) {
    sema_.lateClasses().makeLateClass(cls);
  }
  // The static fields get their values when the declaration runs, in order;
  // the class exists by then (`static instance = new C()`).
  sema_.lateClasses().checkStaticFields(cls);
  ClassDecl *outer = std::exchange(sema_.currentClass_, cls);
  // Then the decorators, as TypeScript applies them right after the class:
  // of a generic class once, for the generic class itself (with the types
  // of the declaration, as TypeScript, which has one class), not for each
  // instance.
  if (!cls->generic && hasDecorators(cls)) {
    sema_.decoratorChecker().decorateClass(cls);
  }
  sema_.currentClass_ = outer;
}

bool ClassChecker::hasDecorators(const ClassDecl *cls) {
  auto decorated = [](const FunctionDecl *fn) {
    return !fn->decorators.empty() ||
           llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
  };

  return !cls->decorators.empty() || llvm::any_of(cls->methods, decorated) ||
         llvm::any_of(cls->constructors, decorated) ||
         llvm::any_of(cls->fields, [](const ClassField &f) { return !f.decorators.empty(); });
}

void ClassChecker::checkClassBodies(ClassDecl *cls) {
  sema_.classDeclarer().completeClass(cls);
  if (cls->init && !cls->init->checked) {
    sema_.flow_ = Sema::Flow();
    sema_.functionChecker().checkFunctionBody(cls->init);
  }
  // Without a constructor, the initializers must set every field.
  if (cls->constructors.empty()) {
    for (const ClassField &f : cls->fields) {
      if (f.isStatic || f.method || f.optional || f.init || f.definite || !f.resolved ||
          f.resolved->isError()) {
        continue;
      }
      sema_.diags_.error(f.loc, "field '" + f.name + "' never gets a value: give it one here, e.g. '" +
                              f.name + ": " + f.resolved->name() +
                              " = ...', set it in a "
                              "constructor, or make it optional: '" +
                              f.name + "?: " + f.resolved->name() + "'");
    }
  }
  for (FunctionDecl *ctor : cls->constructors) {
    if (!ctor->checked) {
      sema_.flow_ = Sema::Flow();
      sema_.functionChecker().checkFunctionBody(ctor);
    }
  }
  std::vector<FunctionDecl *> methods = cls->methods;
  for (const ClassField &f : cls->fields) {
    if (f.method) {
      methods.push_back(f.method);
    }
  }
  for (FunctionDecl *fn : methods) {
    if (sema_.lateClasses().isTemplate(fn->sym)) { // checked in its copies
      continue;
    }
    if (!fn->sym->ret) {
      sema_.returnInferrer().inferReturnType(fn->sym);
    }
    if (fn->checked || fn->isAbstract) {
      continue;
    }
    sema_.flow_ = Sema::Flow();
    sema_.functionChecker().checkFunctionBody(fn);
  }
  sema_.inheritChecker().checkAbstract(cls);
  sema_.inheritChecker().checkImplements(cls);
  // An override returns what the method it overrides returns, or, for an
  // object, a type that is one (Dog for Animal): the same value in memory.
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    if (!fn->overrides) {
      continue;
    }
    const Type *mine = sema_.returnInferrer().returnType(fn->sym), *theirs = sema_.returnInferrer().returnType(fn->overrides->sym);
    if (!mine || !theirs || mine == theirs || mine->isError() || theirs->isError()) {
      continue;
    }
    if (mine->nonOptional()->isObject() && theirs->nonOptional()->isObject() &&
        TypeRelations::isImplicitlyConvertible(mine, theirs)) {
      continue;
    }
    sema_.diags_.error(fn->loc(), "'" + fn->name + "' returns '" + mine->name() + "' in '" + cls->name +
                                "', and '" + theirs->name() + "' in '" + fn->overrides->cls->name +
                                "', which it overrides; an override returns what the method it "
                                "overrides returns");
  }
}

VarSymbol *ClassChecker::declareThis(FunctionDecl *fn) {
  VarSymbol *self = sema_.ctx_.newVar();
  self->name = "this";
  self->loc = fn->loc();
  self->type = fn->cls->type;
  self->isConst = true;
  self->owner = fn;
  self->declSeq = ++sema_.seq_;
  fn->thisVar = self;

  return self;
}

// `this.x` and `this.m()` in a constructor see the fields set so far.
bool ClassChecker::isInitThis(const Expr *expr) const {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (conv->isImplicit()) {
      expr = conv->operand;
    } else {
      break;
    }
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->var && sema_.initThis_ && name->var == sema_.initThis_ && sema_.currentDecl() == sema_.initFn_;
}

bool ClassChecker::thisReady(const Sema::Flow &flow) const {
  return !flow.beforeSuper && firstUnset(flow).empty();
}

std::string ClassChecker::firstUnset(const Sema::Flow &flow) const {
  if (!sema_.initFn_) {
    return "";
  }
  for (const ClassField &f : sema_.initFn_->cls->fields) {
    if (!f.isStatic && !f.method && !f.optional && !llvm::is_contained(flow.setFields, f.name)) {
      return f.name.str();
    }
  }

  return "";
}

void ClassChecker::checkFieldsSet(llvm::SMLoc loc) {
  if (sema_.flow_.unreachable) {
    return;
  }
  std::string unset = firstUnset(sema_.flow_);
  if (unset.empty()) {
    return;
  }
  sema_.diags_.error(loc, "the constructor of '" + sema_.initFn_->cls->name + "' does not set field '" + unset +
                        "' on every path; set it, give it a value where it is declared, or "
                        "make it optional: '" +
                        unset + "?: T'");
}

Expr *ClassChecker::checkThis(NameExpr *name) {
  name->type = Type::error();
  // An arrow sees the `this` of the function around it.
  FunctionDecl *method = nullptr;
  for (FunctionDecl *fn : llvm::reverse(sema_.functions_)) {
    if (fn->takesThis()) {
      method = fn;
      break;
    }
    if (fn->isArrow) {
      continue;
    }
    // `function (this: unknown, ...)`: its parameter.
    if (fn->sym && fn->sym->hasThis && !fn->params.empty() && fn->params[0].sym) {
      VarSymbol *var = fn->params[0].sym;
      name->var = var;
      sema_.nestedFunctions().noteUse(var, name->loc());
      name->type = var->type;
      if (const Type *narrowed = sema_.genericChecker().narrowedType(var)) {
        return sema_.typeRelations().convert(name, narrowed);
      }
      return name;
    }
    if (fn->cls && fn->isStatic) {
      sema_.diags_.error(name->loc(), "in a static method 'this' would be the class itself; use the "
                                "class's name: '" +
                                    fn->cls->name + "." +
                                    (sema_.thisMember_.empty() ? "x" : sema_.thisMember_) + "'");
    } else {
      sema_.diags_.error(name->loc(), "a 'function' has no 'this' of its own; use an arrow function, "
                                "which sees the 'this' of the method around it");
    }
    return name;
  }
  if (!method) {
    if (sema_.staticInit_ && sema_.currentClass_) {
      sema_.diags_.error(name->loc(), "in a static field 'this' would be the class itself; use the "
                                "class's name: '" +
                                    sema_.currentClass_->name + "." +
                                    (sema_.thisMember_.empty() ? "x" : sema_.thisMember_) + "'");
    } else {
      sema_.diags_.error(name->loc(), "'this' is used outside of a method; it exists in the methods "
                                "of classes and object literals");
    }
    return name;
  }
  VarSymbol *var = method->thisVar;
  if (!var) {
    return name; // a copy whose body is not being checked
  }
  name->var = var;
  sema_.nestedFunctions().noteUse(var, name->loc());
  name->type = var->type;
  if (var != sema_.initThis_) {
    return name;
  }
  // The rules while the constructor sets the fields.
  if (sema_.currentDecl() != sema_.initFn_) {
    if (!sema_.closureThisReady_) {
      sema_.diags_.error(name->loc(), "this function uses 'this', and it is created before every field "
                                "is set; create it after the constructor sets them");
    }
  } else if (sema_.flow_.beforeSuper) {
    sema_.diags_.error(name->loc(), "'this' cannot be used before 'super(...)': the object exists once "
                              "the constructor of '" +
                                  sema_.initFn_->cls->parent->name + "' has run");
    name->type = Type::error();
  } else if (sema_.thisMember_.empty()) {
    std::string unset = firstUnset(sema_.flow_);
    if (!unset.empty()) {
      sema_.diags_.error(name->loc(), "'this' is used before every field is set: field '" + unset +
                                    "' is not set yet; until then 'this' can only set fields "
                                    "and read the ones set");
    }
  }

  return name;
}

} // namespace tz
