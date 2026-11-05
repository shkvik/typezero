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

ClassChecker::ClassChecker(Checker &checker)
    : checker_(checker) {}

Result<ClassDecl *, ClassCheckerTopLevelClassError>
ClassChecker::topLevelClass(ClassDecl *cls) const {
  bool topLevel = checker_.scopes_.size() == 1 && checker_.functions_.empty() && cls->type;
  if (!topLevel) {
    return err(ClassCheckerTopLevelClassError{
        .code = ClassCheckerTopLevelClassErrorCode::NOT_TOP_LEVEL,
        .message = "a class can be declared only at the top level of a file; here, "
                   "make it with a class expression, 'const " +
                   cls->name.str() + " = class { ... }'",
    });
  }

  return ok(cls);
}

void ClassChecker::checkClassStmt(ClassDecl *cls) {
  Result<ClassDecl *, ClassCheckerTopLevelClassError> placed = topLevelClass(cls);
  if (placed.isErr()) {
    checker_.diags_.error(cls->loc(), placed.error().message);
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

Result<const ClassField *, ClassCheckerFieldStartValueError>
ClassChecker::fieldStartValue(const ClassDecl *cls, const ClassField &field) const {
  bool needsValue = !field.isStatic && !field.method && !field.optional && !field.init &&
                    !field.definite && field.resolved && !field.resolved->isError();
  if (needsValue && ClassLinks::constructorsOf(cls).empty()) {
    std::string name = field.name.str();
    std::string type = field.resolved->name().str();
    return err(ClassCheckerFieldStartValueError{
        .code = ClassCheckerFieldStartValueErrorCode::NO_START_VALUE,
        .message = "field '" + name + "' has no value to start with: '" + type +
                   "' has no empty value, like \"\" or 0; give it one here, e.g. '" + name + ": " +
                   type + " = ...', set it in a constructor, or make it optional: '" + name +
                   "?: " + type + "'",
    });
  }
  if (needsValue) {
    std::string name = field.name.str();
    std::string type = field.resolved->name().str();
    return err(ClassCheckerFieldStartValueError{
        .code = ClassCheckerFieldStartValueErrorCode::NEVER_SET,
        .message = "field '" + name + "' never gets a value: give it one here, e.g. '" + name +
                   ": " + type + " = ...', set it in a constructor, or make it optional: '" + name +
                   "?: " + type + "'",
    });
  }

  return ok(&field);
}

Result<const FunctionDecl *, ClassCheckerOverrideReturnError>
ClassChecker::overrideReturn(const ClassDecl *cls, const FunctionDecl *method) {
  const FunctionDecl *overridden = method->overrides;
  const Type *mine = overridden ? checker_.returnInferrer().returnType(method->sym) : nullptr;
  const Type *theirs = overridden ? checker_.returnInferrer().returnType(overridden->sym) : nullptr;
  bool known = mine && theirs && !mine->isError() && !theirs->isError();
  bool narrowerObject = known && mine->nonOptional()->isObject() &&
                        theirs->nonOptional()->isObject() &&
                        TypeRelations::isImplicitlyConvertible(mine, theirs);
  if (known && mine != theirs && !narrowerObject) {
    return err(ClassCheckerOverrideReturnError{
        .code = ClassCheckerOverrideReturnErrorCode::RETURNS_OTHER_TYPE,
        .message = "'" + method->name.str() + "' returns '" + mine->name().str() + "' in '" +
                   cls->name.str() + "', and '" + theirs->name().str() + "' in '" +
                   overridden->cls->name.str() +
                   "', which it overrides; an override returns what the method it "
                   "overrides returns",
    });
  }

  return ok(method);
}

void ClassChecker::checkClassBodies(ClassDecl *cls) {
  checker_.classDeclarer().completeClass(cls);
  if (cls->init && !cls->init->checked) {
    checker_.flow_ = Checker::Flow();
    checker_.functionChecker().checkFunctionBody(cls->init);
  }
  if (cls->constructors.empty()) {
    for (const ClassField &f : cls->fields) {
      Result<const ClassField *, ClassCheckerFieldStartValueError> started =
          fieldStartValue(cls, f);
      if (started.isErr()) {
        checker_.diags_.error(f.loc, started.error().message);
      }
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
    Result<const FunctionDecl *, ClassCheckerOverrideReturnError> returned =
        overrideReturn(cls, fn);
    if (returned.isErr()) {
      checker_.diags_.error(fn->loc(), returned.error().message);
    }
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

Result<const FunctionDecl *, ClassCheckerFieldsSetError> ClassChecker::fieldsSet() const {
  std::string unset = checker_.flow_.unreachable ? "" : firstUnset(checker_.flow_);
  if (!unset.empty()) {
    return err(ClassCheckerFieldsSetError{
        .code = ClassCheckerFieldsSetErrorCode::FIELD_NOT_SET_ON_EVERY_PATH,
        .message = "the constructor of '" + checker_.initFn_->cls->name.str() +
                   "' does not set field '" + unset +
                   "' on every path; set it, give it a value where it is declared, or "
                   "make it optional: '" +
                   unset + "?: T'",
    });
  }

  return ok(checker_.initFn_);
}

void ClassChecker::checkFieldsSet(llvm::SMLoc loc) {
  Result<const FunctionDecl *, ClassCheckerFieldsSetError> set = fieldsSet();
  if (set.isErr()) {
    checker_.diags_.error(loc, set.error().message);
  }
}

Result<const FunctionDecl *, ClassCheckerThisOfError>
ClassChecker::thisOf(const FunctionDecl *current) const {
  std::string member = checker_.thisMember_.empty() ? "x" : checker_.thisMember_.str();
  const FunctionDecl *source = current ? current->thisSource() : nullptr;
  if (!source && checker_.staticInit_ && checker_.currentClass_) {
    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_STATIC_FIELD,
        .message = "in a static field 'this' would be the class itself; use the "
                   "class's name: '" +
                   checker_.currentClass_->name.str() + "." + member + "'",
    });
  }
  if (!source) {
    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::OUTSIDE_METHOD,
        .message = "'this' is used outside of a method; it exists in the methods "
                   "of classes and object literals",
    });
  }

  bool hasThis = source->takesThis() || source->hasThisParam();
  if (!hasThis && source->cls && source->isStatic) {
    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_STATIC_METHOD,
        .message = "in a static method 'this' would be the class itself; use the "
                   "class's name: '" +
                   source->cls->name.str() + "." + member + "'",
    });
  }
  if (!hasThis) {
    return err(ClassCheckerThisOfError{
        .code = ClassCheckerThisOfErrorCode::IN_PLAIN_FUNCTION,
        .message = "a 'function' has no 'this' of its own; use an arrow function, "
                   "which sees the 'this' of the method around it",
    });
  }

  return ok(source);
}

Result<VarSymbol *, ClassCheckerReadyThisError> ClassChecker::readyThis(VarSymbol *var) const {
  bool inInit = var == checker_.initThis_;
  bool inConstructor = inInit && checker_.currentDecl() == checker_.initFn_;
  bool inClosure = inInit && checker_.currentDecl() != checker_.initFn_;
  if (inClosure && !checker_.closureThisReady_) {
    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::IN_CLOSURE_BEFORE_FIELDS_SET,
        .message = "this function uses 'this', and it is created before every field "
                   "is set; create it after the constructor sets them",
    });
  }
  if (inConstructor && checker_.flow_.beforeSuper) {
    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_SUPER,
        .message = "'this' cannot be used before 'super(...)': the object exists once "
                   "the constructor of '" +
                   checker_.initFn_->cls->parent->name.str() + "' has run",
    });
  }

  bool fieldsMatter = inConstructor && checker_.thisMember_.empty();
  std::string unset = fieldsMatter ? firstUnset(checker_.flow_) : "";
  if (!unset.empty()) {
    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_FIELDS_SET,
        .message = "'this' is used before every field is set: field '" + unset +
                   "' is not set yet; until then 'this' can only set fields "
                   "and read the ones set",
    });
  }

  return ok(var);
}

Expr *ClassChecker::checkThis(NameExpr *name) {
  name->type = Type::error();
  Result<const FunctionDecl *, ClassCheckerThisOfError> found = thisOf(checker_.currentDecl());
  if (found.isErr()) {
    checker_.diags_.error(name->loc(), found.error().message);
    return name;
  }

  const FunctionDecl *source = found.value();
  VarSymbol *var = source->takesThis() ? source->thisVar : source->params[0].sym;
  if (!var) {
    return name;
  }
  name->var = var;
  checker_.nestedFunctions().noteUse(var, name->loc());
  name->type = var->type;
  if (!source->takesThis()) {
    if (const Type *narrowed = checker_.genericChecker().narrowedType(var)) {
      return checker_.typeRelations().convert(name, narrowed);
    }
    return name;
  }

  Result<VarSymbol *, ClassCheckerReadyThisError> ready = readyThis(var);
  if (ready.isErr()) {
    checker_.diags_.error(name->loc(), ready.error().message);
  }
  if (ready.isErr() && ready.error().code == ClassCheckerReadyThisErrorCode::BEFORE_SUPER) {
    name->type = Type::error();
  }

  return name;
}

}
