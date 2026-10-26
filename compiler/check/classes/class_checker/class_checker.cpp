#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/classes/inherit_checker/inherit_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

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

ClassChecker::ClassChecker(Checker &checker) : checker_(checker) {}

void ClassChecker::checkClassStmt(ClassDecl *cls) {
  if (checker_.scopes_.size() != 1 || !checker_.functions_.empty() || !cls->type) {
    checker_.diags_.error(cls->loc(),
                          "a class can be declared only at the top level of a file; here, "
                          "make it with a class expression, 'const " +
                              cls->name + " = class { ... }'");
    return;
  }
  if (cls->baseExpr && !cls->resolved) {
    checker_.lateClasses().makeLateClass(cls);
  }
  checker_.lateClasses().checkStaticFields(cls);
  ClassDecl *outer = std::exchange(checker_.currentClass_, cls);
  if (!cls->generic && hasDecorators(cls)) {
    checker_.decoratorChecker().decorateClass(cls);
  }
  checker_.currentClass_ = outer;
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
  checker_.classDeclarer().completeClass(cls);
  if (cls->init && !cls->init->checked) {
    checker_.flow_ = Checker::Flow();
    checker_.functionChecker().checkFunctionBody(cls->init);
  }
  if (cls->constructors.empty()) {
    for (const ClassField &f : cls->fields) {
      if (f.isStatic || f.method || f.optional || f.init || f.definite || !f.resolved ||
          f.resolved->isError()) {
        continue;
      }
      checker_.diags_.error(f.loc, "field '" + f.name +
                                       "' never gets a value: give it one here, e.g. '" + f.name +
                                       ": " + f.resolved->name() +
                                       " = ...', set it in a "
                                       "constructor, or make it optional: '" +
                                       f.name + "?: " + f.resolved->name() + "'");
    }
  }
  for (FunctionDecl *ctor : cls->constructors) {
    if (!ctor->checked) {
      checker_.flow_ = Checker::Flow();
      checker_.functionChecker().checkFunctionBody(ctor);
    }
  }
  std::vector<FunctionDecl *> methods = cls->methods;
  for (const ClassField &f : cls->fields) {
    if (f.method) {
      methods.push_back(f.method);
    }
  }
  for (FunctionDecl *fn : methods) {
    if (checker_.lateClasses().isTemplate(fn->sym)) {
      continue;
    }
    if (!fn->sym->ret) {
      checker_.returnInferrer().inferReturnType(fn->sym);
    }
    if (fn->checked || fn->isAbstract) {
      continue;
    }
    checker_.flow_ = Checker::Flow();
    checker_.functionChecker().checkFunctionBody(fn);
  }
  checker_.inheritChecker().checkAbstract(cls);
  checker_.inheritChecker().checkImplements(cls);
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    if (!fn->overrides) {
      continue;
    }
    const Type *mine = checker_.returnInferrer().returnType(fn->sym),
               *theirs = checker_.returnInferrer().returnType(fn->overrides->sym);
    if (!mine || !theirs || mine == theirs || mine->isError() || theirs->isError()) {
      continue;
    }
    if (mine->nonOptional()->isObject() && theirs->nonOptional()->isObject() &&
        TypeRelations::isImplicitlyConvertible(mine, theirs)) {
      continue;
    }
    checker_.diags_.error(fn->loc(),
                          "'" + fn->name + "' returns '" + mine->name() + "' in '" + cls->name +
                              "', and '" + theirs->name() + "' in '" + fn->overrides->cls->name +
                              "', which it overrides; an override returns what the method it "
                              "overrides returns");
  }
}

VarSymbol *ClassChecker::declareThis(FunctionDecl *fn) {
  VarSymbol *self = checker_.ctx_.newVar();
  self->name = "this";
  self->loc = fn->loc();
  self->type = fn->cls->type;
  self->isConst = true;
  self->owner = fn;
  self->declSeq = ++checker_.seq_;
  fn->thisVar = self;

  return self;
}

bool ClassChecker::isInitThis(const Expr *expr) const {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (conv->isImplicit()) {
      expr = conv->operand;
    } else {
      break;
    }
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->var && checker_.initThis_ && name->var == checker_.initThis_ &&
         checker_.currentDecl() == checker_.initFn_;
}

bool ClassChecker::thisReady(const Checker::Flow &flow) const {
  return !flow.beforeSuper && firstUnset(flow).empty();
}

std::string ClassChecker::firstUnset(const Checker::Flow &flow) const {
  if (!checker_.initFn_) {
    return "";
  }
  for (const ClassField &f : checker_.initFn_->cls->fields) {
    if (!f.isStatic && !f.method && !f.optional && !llvm::is_contained(flow.setFields, f.name)) {
      return f.name.str();
    }
  }

  return "";
}

void ClassChecker::checkFieldsSet(llvm::SMLoc loc) {
  if (checker_.flow_.unreachable) {
    return;
  }
  std::string unset = firstUnset(checker_.flow_);
  if (unset.empty()) {
    return;
  }
  checker_.diags_.error(
      loc, "the constructor of '" + checker_.initFn_->cls->name + "' does not set field '" + unset +
               "' on every path; set it, give it a value where it is declared, or "
               "make it optional: '" +
               unset + "?: T'");
}

Expr *ClassChecker::checkThis(NameExpr *name) {
  name->type = Type::error();
  FunctionDecl *method = nullptr;
  for (FunctionDecl *fn : llvm::reverse(checker_.functions_)) {
    if (fn->takesThis()) {
      method = fn;
      break;
    }
    if (fn->isArrow) {
      continue;
    }
    if (fn->sym && fn->sym->hasThis && !fn->params.empty() && fn->params[0].sym) {
      VarSymbol *var = fn->params[0].sym;
      name->var = var;
      checker_.nestedFunctions().noteUse(var, name->loc());
      name->type = var->type;
      if (const Type *narrowed = checker_.genericChecker().narrowedType(var)) {
        return checker_.typeRelations().convert(name, narrowed);
      }
      return name;
    }
    if (fn->cls && fn->isStatic) {
      checker_.diags_.error(name->loc(),
                            "in a static method 'this' would be the class itself; use the "
                            "class's name: '" +
                                fn->cls->name + "." +
                                (checker_.thisMember_.empty() ? "x" : checker_.thisMember_) + "'");
    } else {
      checker_.diags_.error(name->loc(),
                            "a 'function' has no 'this' of its own; use an arrow function, "
                            "which sees the 'this' of the method around it");
    }
    return name;
  }
  if (!method) {
    if (checker_.staticInit_ && checker_.currentClass_) {
      checker_.diags_.error(name->loc(),
                            "in a static field 'this' would be the class itself; use the "
                            "class's name: '" +
                                checker_.currentClass_->name + "." +
                                (checker_.thisMember_.empty() ? "x" : checker_.thisMember_) + "'");
    } else {
      checker_.diags_.error(name->loc(),
                            "'this' is used outside of a method; it exists in the methods "
                            "of classes and object literals");
    }
    return name;
  }
  VarSymbol *var = method->thisVar;
  if (!var) {
    return name;
  }
  name->var = var;
  checker_.nestedFunctions().noteUse(var, name->loc());
  name->type = var->type;
  if (var != checker_.initThis_) {
    return name;
  }
  if (checker_.currentDecl() != checker_.initFn_) {
    if (!checker_.closureThisReady_) {
      checker_.diags_.error(name->loc(),
                            "this function uses 'this', and it is created before every field "
                            "is set; create it after the constructor sets them");
    }
  } else if (checker_.flow_.beforeSuper) {
    checker_.diags_.error(name->loc(),
                          "'this' cannot be used before 'super(...)': the object exists once "
                          "the constructor of '" +
                              checker_.initFn_->cls->parent->name + "' has run");
    name->type = Type::error();
  } else if (checker_.thisMember_.empty()) {
    std::string unset = firstUnset(checker_.flow_);
    if (!unset.empty()) {
      checker_.diags_.error(name->loc(),
                            "'this' is used before every field is set: field '" + unset +
                                "' is not set yet; until then 'this' can only set fields "
                                "and read the ones set");
    }
  }

  return name;
}

}
